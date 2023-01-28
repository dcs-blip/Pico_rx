#include "rx_dsp.h"
#include "rx_definitions.h"
#include "utils.h"
#include <math.h>
#include <cstdio>
#include "pico/stdlib.h"

uint16_t rx_dsp :: process_block(uint16_t samples[], int16_t audio_samples[])
{

  uint16_t odx = 0;
  int32_t magnitude_sum = 0;
  for(uint16_t idx=0; idx<adc_block_size; idx++)
  {
      //convert to signed representation
      const int16_t raw_sample = samples[idx] - (1<<(adc_bits-1));

      //work out which samples are i and q
      int16_t i = (idx&1^1)*raw_sample;//even samples contain i data
      int16_t q = (idx&1)*raw_sample;//odd samples contain q data

      //capture data for spectrum
      if(idx < 256)
      {
        capture_i[idx] = i>>4;//only use 8 msbs
        capture_q[idx] = q>>4;//only use 8 msbs
      }

      //Apply frequency shift (move tuned frequency to DC)         
      frequency_shift(i, q);

      //decimate by factor of 40
      if(decimate(i, q))
      {

        //Measure amplitude (for signal strength indicator)
        int32_t amplitude = rectangular_2_magnitude(i, q);
        magnitude_sum += amplitude;

        //Demodulate to give audio sample
        int32_t audio = demodulate(i, q);

        //Automatic gain control scales signal to use full 16 bit range
        //e.g. -32767 to 32767
        audio = automatic_gain_control(audio);

        //convert to unsigned value in range 0 to 500 to output to PWM
        audio += INT16_MAX;
        audio /= pwm_scale; 

        for(uint8_t sample=0; sample < interpolation_rate; sample++)
        {
          audio_samples[odx] = audio;
          odx++;
        }
          
      }
    } 

    //average over the number of samples
    signal_amplitude = (magnitude_sum * total_decimation_rate)/adc_block_size;

    return odx;
}

void rx_dsp :: frequency_shift(int16_t &i, int16_t &q)
{
    //Apply frequency shift (move tuned frequency to DC)         
    const int16_t rotation_i =  cos_table[phase>>22]; //32 - 22 = 10MSBs
    const int16_t rotation_q = -sin_table[phase>>22];

    phase += frequency;
    const int16_t i_shifted = ((i * rotation_i) - (q * rotation_q)) >> 15;
    const int16_t q_shifted = ((q * rotation_i) + (i * rotation_q)) >> 15;

    i = i_shifted;
    q = q_shifted;
}

bool rx_dsp :: decimate(int16_t &i, int16_t &q)
{
      //Decimate
      //             fs          Alias Free
      //raw data    500kHz
      //CIC (20)    12.5kHz      +/-6.25kHz
      //filt1       15.625kHz    +/-3.125kHz(with aliases outside)
      //filt2       15.625kHz    +/-3.125kHz(free from aliases)

      //CIC decimation filter
      //implement integrator stages
      integratori1 += i;
      integratorq1 += q;
      integratori2 += integratori1;
      integratorq2 += integratorq1;
      integratori3 += integratori2;
      integratorq3 += integratorq2;
      integratori4 += integratori3;
      integratorq4 += integratorq3;

      decimate_count++;
      if(decimate_count == decimation_rate)
      {
        decimate_count = 0;

        //implement comb stages
        const int32_t combi1 = integratori4-delayi0;
        const int32_t combq1 = integratorq4-delayq0;
        const int32_t combi2 = combi1-delayi1;
        const int32_t combq2 = combq1-delayq1;
        const int32_t combi3 = combi2-delayi2;
        const int32_t combq3 = combq2-delayq2;
        const int32_t combi4 = combi3-delayi3;
        const int32_t combq4 = combq3-delayq3;
        delayi0 = integratori4;
        delayq0 = integratorq4;
        delayi1 = combi1;
        delayq1 = combq1;
        delayi2 = combi2;
        delayq2 = combq2;
        delayi3 = combi3;
        delayq3 = combq3;
        int16_t decimated_i = combi4>>(growth-3);
        int16_t decimated_q = combq4>>(growth-3);

        //first half band decimating filter
        bool new_sample = half_band_filter_inst.filter(decimated_i, decimated_q);

        //second half band filter (not decimating)
        if(new_sample)
        {
           half_band_filter2_inst.filter(decimated_i, decimated_q);
           i = decimated_i;
           q = decimated_q;
           return true;
        }
      }

      return false;
}

int16_t rx_dsp :: demodulate(int16_t i, int16_t q)
{
    if(mode == AM)
    {
        int16_t amplitude = rectangular_2_magnitude(i, q);
        //measure DC using first order IIR low-pass filter
        audio_dc = amplitude+(audio_dc - (audio_dc >> 5));
        //subtract DC component
        return amplitude - (audio_dc >> 5);
    }
    else if(mode == FM)
    {
        int16_t audio_phase = rectangular_2_phase(i, q);
        int16_t frequency = audio_phase - last_audio_phase;
        last_audio_phase = audio_phase;
        return frequency;
    }
    else if(mode == LSB || mode == USB)
    {
        //shift frequency by +FS/4
        //      __|__
        //  ___/  |  \___
        //        |
        //  <-----+----->

        //        | ____
        //  ______|/    \
        //        |
        //  <-----+----->

        //filter -Fs/4 to +Fs/4

        //        | __  
        //  ______|/  \__
        //        |
        //  <-----+----->


        if(mode == USB)
        {
          ssb_phase = (ssb_phase + 1) & 3u;
        }
        else
        {
          ssb_phase = (ssb_phase - 1) & 3u;
        }

        const int16_t sample_i[4] = {i, q, -i, -q};
        const int16_t sample_q[4] = {q, -i, -q, i};
        int16_t ii = sample_i[ssb_phase];
        int16_t qq = sample_q[ssb_phase];
        ssb_filter.filter(ii,  qq);

        //shift frequency by -FS/4 and discard q to form a real (not complex) sample
        //        | __  
        //  ______|/  \__
        //        |
        //  <-----+----->

        //     __ |     
        //  __/  \|______
        //        |
        //  <-----+----->

        //     __ | __   
        //  __/  \|/  \__
        //        |
        //  <-----+----->

        const int16_t audio[4] = {-qq, -ii, qq, ii};
        return audio[ssb_phase];

    }
    else //if(mode==cw)
    {
      int16_t ii = i;
      int16_t qq = q;
      if(cw_decimate(ii, qq)){
        cw_magnitude = rectangular_2_magnitude(ii, qq);
      }
      cw_sidetone_phase += cw_sidetone_frequency_Hz * 1024 * total_decimation_rate / adc_sample_rate;
      return ((int32_t)cw_magnitude * sin_table[cw_sidetone_phase & 0x3ff])>>15;
    }
}

bool rx_dsp :: cw_decimate(int16_t &i, int16_t &q)
{
      //CIC decimation filter
      //implement integrator stages
      cw_integratori1 += i;
      cw_integratorq1 += q;
      cw_integratori2 += cw_integratori1;
      cw_integratorq2 += cw_integratorq1;
      cw_integratori3 += cw_integratori2;
      cw_integratorq3 += cw_integratorq2;
      cw_integratori4 += cw_integratori3;
      cw_integratorq4 += cw_integratorq3;

      cw_decimate_count++;
      if(cw_decimate_count == cw_decimation_rate)
      {
        cw_decimate_count = 0;

        //implement comb stages
        const int32_t combi1 = integratori4-delayi0;
        const int32_t combq1 = integratorq4-delayq0;
        const int32_t combi2 = combi1-delayi1;
        const int32_t combq2 = combq1-delayq1;
        const int32_t combi3 = combi2-delayi2;
        const int32_t combq3 = combq2-delayq2;
        const int32_t combi4 = combi3-delayi3;
        const int32_t combq4 = combq3-delayq3;
        cw_delayi0 = integratori4;
        cw_delayq0 = integratorq4;
        cw_delayi1 = combi1;
        cw_delayq1 = combq1;
        cw_delayi2 = combi2;
        cw_delayq2 = combq2;
        cw_delayi3 = combi3;
        cw_delayq3 = combq3;
        int16_t decimated_i = combi4>>growth;
        int16_t decimated_q = combq4>>growth;

        //first half band decimating filter
        bool new_sample = cw_half_band_filter_inst.filter(decimated_i, decimated_q);

        //second half band filter (not decimating)
        if(new_sample)
        {
           cw_half_band_filter2_inst.filter(decimated_i, decimated_q);
           i = decimated_i;
           q = decimated_q;
           return true;
        }
      }

      return false;
}

int16_t rx_dsp::automatic_gain_control(int16_t audio_in)
{
    //Use a leaky max hold to estimate audio power
    //             _
    //            | |
    //            | |
    //    audio __| |_____________________
    //            | |
    //            |_|
    //
    //                _____________
    //               /             \_
    //    max_hold  /                \_
    //           _ /                   \_
    //              ^                ^
    //            attack             |
    //                <---hang--->   |
    //                             decay

    // Attack is fast so that AGC reacts fast to increases in power
    // Hang time and decay are relatively slow to prevent rapid gain changes

    static const uint8_t extra_bits = 16;
    int32_t audio = audio_in;
    const int32_t audio_scaled = audio << extra_bits;
    if(audio_scaled > max_hold)
    {
      //attack
      max_hold += (audio_scaled - max_hold) >> attack_factor;
      hang_timer = hang_time;
    }
    else if(hang_timer)
    {
      //hang
      hang_timer--;
    }
    else if(max_hold > 0)
    {
      //decay
      max_hold -= max_hold>>decay_factor; 
    }

    //calculate gain needed to amplify to full scale
    const int16_t magnitude = max_hold >> extra_bits;
    const int16_t limit = INT16_MAX; //hard limit
    const int16_t setpoint = limit/2; //about half full scale

    //apply gain
    if(magnitude > 0)
    {
      const int16_t gain = setpoint/magnitude;
      audio *= gain;
    }

    //soft clip (compress)
    if (audio > setpoint)  audio =  setpoint + ((audio-setpoint)>>1);
    if (audio < -setpoint) audio = -setpoint - ((audio+setpoint)>>1);

    //hard clamp
    if (audio > limit)  audio = limit;
    if (audio < -limit) audio = -limit;

    return audio;
}

rx_dsp :: rx_dsp()
{

  //initialise state
  dc = 0;
  phase = 0;
  decimate_count=0;
  frequency=0;
  initialise_luts();

  //clear cic filter
  integratori1=0; integratorq1=0;
  integratori2=0; integratorq2=0;
  integratori3=0; integratorq3=0;
  integratori4=0; integratorq4=0;
  delayi0=0; delayq0=0;
  delayi1=0; delayq1=0;
  delayi2=0; delayq2=0;
  delayi3=0; delayq3=0;

  //clear cw filter
  cw_integratori1=0; cw_integratorq1=0;
  cw_integratori2=0; cw_integratorq2=0;
  cw_integratori3=0; cw_integratorq3=0;
  cw_integratori4=0; cw_integratorq4=0;
  cw_delayi0=0; cw_delayq0=0;
  cw_delayi1=0; cw_delayq1=0;
  cw_delayi2=0; cw_delayq2=0;
  cw_delayi3=0; cw_delayq3=0;

  set_agc_speed(3);

}

void rx_dsp :: set_agc_speed(uint8_t agc_setting)
{
  //Configure AGC
  // input fs=500000.000000 Hz
  // decimation=20 x 2
  // fs=12500.000000 Hz
  // Setting Decay Time(s) Factor Attack Time(s) Factor  Hang  Timer
  // ======= ============= ====== ============== ======  ====  =====
  // fast        0.047       9        0.001         2    0.1s   1250
  // medium      0.189       10       0.001         2    0.25s  3125
  // slow        0.377       11       0.001         2    1s     12500
  // long        1.509       13       0.001         2    2s     25000


  switch(agc_setting)
  {
      case 0: //fast
        attack_factor=2;
        decay_factor=9;
        hang_time=1250;
        break;

      case 1: //medium
        attack_factor=2;
        decay_factor=10;
        hang_time=3125;
        break;

      case 2: //slow
        attack_factor=2;
        decay_factor=11;
        hang_time=12500;
        break;

      default: //long
        attack_factor=2;
        decay_factor=13;
        hang_time=25000;
        break;
  }
}

void rx_dsp :: set_frequency_offset_Hz(double offset_frequency)
{
  offset_frequency_Hz = offset_frequency;
  frequency = ((double)(1ull<<32)*offset_frequency)/adc_sample_rate;
}

void rx_dsp :: set_mode(uint8_t val)
{
  mode = val;
}

int32_t rx_dsp :: get_signal_amplitude()
{
  return signal_amplitude;
}

void rx_dsp :: get_spectrum(int16_t spectrum[], int16_t &offset)
{
    //convert capture to frequency domain
    uint16_t f=0;
    clock_t start_time;
    fft(capture_i, capture_q);
    for(uint16_t i=192; i<256; i++) spectrum[f++] = rectangular_2_magnitude(capture_i[i], capture_q[i]);
    for(uint16_t i=0; i<64; i++) spectrum[f++] = rectangular_2_magnitude(capture_i[i], capture_q[i]);
    offset = 64 + ((offset_frequency_Hz*256)/adc_sample_rate);
}
