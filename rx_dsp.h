#ifndef RX_DSP_H
#define RX_DSP_H

#include <stdint.h>
#include "rx_definitions.h"
#include "pico/sem.h"
#include "fft_filter.h"

class rx_dsp
{
  public:

  rx_dsp();
  uint16_t process_block(uint16_t samples[], int16_t audio_samples[]);
  void set_frequency_offset_Hz(double offset_frequency);
  void set_agc_speed(uint8_t agc_setting);
  void set_mode(uint8_t mode, uint8_t bw);
  void set_cw_sidetone_Hz(uint16_t val);
  void set_gain_cal_dB(uint16_t val);
  void set_volume(uint8_t val);
  void set_squelch(uint8_t val);
  void set_swap_iq(uint8_t val);
  void set_pwm_max(uint32_t pwm_max);
  int16_t get_signal_strength_dBm();
  void get_spectrum(float spectrum[], int16_t &offset);


  private:
  
  void frequency_shift(int16_t &i, int16_t &q);
  bool decimate(int16_t &i, int16_t &q);
  int16_t demodulate(int16_t i, int16_t q);
  int16_t automatic_gain_control(int16_t audio);
  bool cw_decimate(int16_t &i, int16_t &q);

  //capture samples for spectral analysis
  int16_t capture_i[256];
  int16_t capture_q[256];
  float accumulator[256];
  semaphore_t spectrum_semaphore;
  bool capture_data = false;
  uint16_t cap;
  uint16_t segment=0;

  //used in dc canceler
  int64_t dc;

  //used in cic decimator
  uint8_t decimate_count;
  int32_t integratori1, integratorq1;
  int32_t integratori2, integratorq2;
  int32_t integratori3, integratorq3;
  int32_t integratori4, integratorq4;
  int32_t delayi0, delayq0;
  int32_t delayi1, delayq1;
  int32_t delayi2, delayq2;
  int32_t delayi3, delayq3;

  //used in fft filter
  fft_filter fft_filter_inst;
  uint16_t start_frequency;
  uint16_t stop_frequency;
  bool lower_sideband;
  bool upper_sideband;

  //used in frequency shifter
  uint8_t swap_iq;
  int32_t offset_frequency_Hz;
  int32_t dither;
  uint32_t phase;
  int32_t frequency;

  //used to generate cw sidetone
  int16_t cw_i, cw_q;
  int16_t cw_sidetone_phase;
  int16_t cw_sidetone_frequency_Hz=1000;

  int32_t signal_amplitude;

  //used in demodulator
  int32_t mode=0;
  int32_t audio_dc=0;
  uint8_t ssb_phase=0;
  int16_t last_phase=0;

  //volume control
  int16_t gain_numerator=0;

  //squelch
  int16_t squelch_threshold=0;
  int16_t s9_threshold=0;

  //used in AGC
  uint8_t attack_factor;
  uint8_t decay_factor;
  uint16_t hang_time;
  uint16_t hang_timer;
  const bool agc_enabled = true;
  int32_t max_hold;
  uint32_t pwm_scale;



  // gain calibration
  float amplifier_gain_dB = 62.0f;

};

#endif
