#include "Arduino.h"

#include "application.h"

#include "hw.h"
#include "mcpDac.h"
#include "ihandlers.h"
#include "timer.h"

const AppMode AppModeValues[] = {MUTE,NORMAL};

static int32_t pitchCalibrationBase = 0;
static int32_t pitchCalibrationBaseFreq = 0;
static int32_t pitchCalibrationConstant = 0;
static int32_t pitchSensitivityConstant = 70000;
static float qMeasurement = 0;

static int32_t volCalibrationBase   = 0;

Application::Application()
  : _state(PLAYING),
    _mode(NORMAL) {
};

void Application::setup() {
#if SERIAL_ENABLED
  Serial.begin(Application::BAUD);
#endif

  HW_LED1_ON;HW_LED2_OFF;

  pinMode(Application::BUTTON_PIN, INPUT_PULLUP);
  pinMode(Application::LED_PIN_1,    OUTPUT);
  pinMode(Application::LED_PIN_2,    OUTPUT);

  digitalWrite(Application::LED_PIN_1, HIGH);    // turn the LED off by making the voltage LOW


   initialiseTimer();
   initialiseInterrupts();

   mcpDacInit();
/// TEST
 
   calibrate_pitch();
   calibrate_volume();


   initialiseTimer();
   initialiseInterrupts();
   

#if CV_ENABLED
  initialiseCVOut();
#endif

  playStartupSound();
  calibrate();

}

void Application::initialiseTimer() {
  ihInitialiseTimer();
}

void Application::initialiseInterrupts() {
  ihInitialiseInterrupts();
}

void Application::InitialisePitchMeasurement() {
   ihInitialisePitchMeasurement();
}

void Application::InitialiseVolumeMeasurement() {
   ihInitialiseVolumeMeasurement();
}

unsigned long Application::GetQMeasurement()
{
  int qn=0;
  
  TCCR1B = (1<<CS10);	

while(!(PIND & (1<<PORTD3)));
while((PIND & (1<<PORTD3)));

TCNT1 = 0;
  timer_overflow_counter = 0;
while(qn<31250){
while(!(PIND & (1<<PORTD3)));
qn++;
while((PIND & (1<<PORTD3)));
};

 
  
  TCCR1B = 0;	

 unsigned long frequency = TCNT1;
 unsigned long temp = 65536*(unsigned long)timer_overflow_counter;
  frequency += temp;

return frequency;

}


unsigned long Application::GetPitchMeasurement()
{
  TCNT1 = 0;
  timer_overflow_counter = 0;
  TCCR1B = (1<<CS12) | (1<<CS11) | (1<<CS10);	

  delay(1000);  
  
  TCCR1B = 0;	

 unsigned long frequency = TCNT1;
 unsigned long temp = 65536*(unsigned long)timer_overflow_counter;
  frequency += temp;

return frequency;

}

unsigned long Application::GetVolumeMeasurement()
{timer_overflow_counter = 0;

  TCNT0=0;
  TCNT1=49911;
  TCCR0B = (1<<CS02) | (1<<CS01) | (1<<CS00);	 // //External clock source on T0 pin. Clock on rising edge.
  TIFR1  = (1<<TOV1);        //Timer1 INT Flag Reg: Clear Timer Overflow Flag

while(!(TIFR1&((1<<TOV1)))); // on Timer 1 overflow (1s)
  TCCR0B = 0;	 // Stop TimerCounter 0
 unsigned long frequency = TCNT0; // get counter 0 value
 unsigned long temp = (unsigned long)timer_overflow_counter; // and overflow counter

 frequency += temp*256;

return frequency;
}



#if CV_ENABLED                                 // Initialise PWM Generator for CV output
void initialiseCVOut() {

}
#endif

AppMode Application::nextMode() {
  return _mode == NORMAL ? MUTE : AppModeValues[_mode + 1];
}

void Application::loop() {
  int32_t pitch_v = 0, pitch_l = 0;            // Last value of pitch  (for filtering)
  int32_t vol_v = 0,   vol_l = 0;              // Last value of volume (for filtering)
  int32_t pitch_f = 0;                        //  pitch in frequency (for filtering)

  uint16_t volumePotValue = 0;
  uint16_t pitchPotValue = 0;



  mloop:                   // Main loop avoiding the GCC "optimization"

  pitchPotValue    = analogRead(PITCH_POT);
  volumePotValue   = analogRead(VOLUME_POT);

  vWavetableSelector = analogRead(WAVE_SELECT_POT) >> 7;


  if (_state == PLAYING && HW_BUTTON_PRESSED) {
    _state = CALIBRATING;
    resetTimer();
  }

  if (_state == CALIBRATING && HW_BUTTON_RELEASED) {
    if (timerExpired(1500)) {
     
         _mode = nextMode();
 if (_mode==NORMAL) {HW_LED1_ON;HW_LED2_OFF;} else {HW_LED1_OFF;HW_LED2_ON;};
   // playModeSettingSound();
   
   
    }
    _state = PLAYING;
  };

  if (_state == CALIBRATING && timerExpired(10000)) {

      HW_LED2_ON;
      playCalibratingCountdownSound();

      calibrate();

      _mode=NORMAL;
      HW_LED2_OFF;
      
    while (HW_BUTTON_PRESSED)
      ; // NOP
    _state = PLAYING;
  };

#if CV_ENABLED
  OCR0A = pitch & 0xff;
#endif

#if SERIAL_ENABLED
  if (timerExpired(TICKS_100_MILLIS)) {
    resetTimer();
    Serial.write(pitch & 0xff);              // Send char on serial (if used)
    Serial.write((pitch >> 8) & 0xff);
  }
#endif

  if (pitchValueAvailable) {                        // If capture event

    pitch_v=pitch;                         // Averaging pitch values
    pitch_v=pitch_l+((pitch_v-pitch_l)>>2);
    pitch_l=pitch_v;

    pitch_f=FREQ_FACTOR/pitch_v;

//HW_LED2_ON;


    // set wave frequency for each mode
    switch (_mode) {
      case MUTE : /* NOTHING! */;                                        break;
      case NORMAL      : setWavetableSampleAdvance((pitchCalibrationBase-pitch_v)/4+2048-(pitchPotValue<<2)); break;
    };
    
  //  HW_LED2_OFF;

    pitchValueAvailable = false;
  }

  if (volumeValueAvailable) {
    vol = max(vol, 5000);

    vol_v=vol;                  // Averaging volume values
    vol_v=vol_l+((vol_v-vol_l)>>2);
    vol_l=vol_v;

    switch (_mode) {
      case MUTE:  vol_v = 0;                                                      break;
      case NORMAL:      vol_v = MAX_VOLUME-(volCalibrationBase-vol_v)/2+(volumePotValue<<2)-1024;                                     break;
    };

    // Limit and set volume value
    vol_v = min(vol_v, 4095);
    //    vol_v = vol_v - (1 + MAX_VOLUME - (volumePotValue << 2));
    vol_v = vol_v ;
    vol_v = max(vol_v, 0);
    vScaledVolume = vol_v >> 4;

    volumeValueAvailable = false;
  }

  goto mloop;                           // End of main loop
}

void Application::calibrate()
{
  resetPitchFlag();
  resetTimer();
  savePitchCounter();
  while (!pitchValueAvailable && timerUnexpiredMillis(10))
    ; // NOP
  pitchCalibrationBase = pitch;
  pitchCalibrationBaseFreq = FREQ_FACTOR/pitchCalibrationBase;
  pitchCalibrationConstant = FREQ_FACTOR/pitchSensitivityConstant/2+200;

  resetVolFlag();
  resetTimer();
  saveVolCounter();
  while (!volumeValueAvailable && timerUnexpiredMillis(10))
    ; // NOP
  volCalibrationBase = vol;
}

void Application::calibrate_pitch()
{
  
static int16_t pitchXn0 = 0;
static int16_t pitchXn1 = 0;
static int16_t pitchXn2 = 0;
static float q0 = 0;
static long pitchfn0 = 0;
static long pitchfn1 = 0;
static long pitchfn = 0;

  Serial.begin(9600);
  Serial.println("Pitch calibration");


  InitialisePitchMeasurement();
  interrupts();
  mcpDacInit();

qMeasurement = GetQMeasurement();
Serial.print("Q ");
Serial.println(qMeasurement);

q0 = (16000000/qMeasurement*500000);
Serial.println(q0);

pitchXn0 = 0;
pitchXn1 = 4095;

pitchfn = q0-600;

mcpDac2BSend(1600);

mcpDac2ASend(pitchXn0);
delay(100);
pitchfn0 = GetPitchMeasurement();

mcpDac2ASend(pitchXn1);
delay(100);
pitchfn1 = GetPitchMeasurement();

Serial.print(pitchfn0);
    Serial.print("  ");
  Serial.println(pitchfn1);
  
 
while(abs(pitchfn0-pitchfn1)>10){

mcpDac2ASend(pitchXn0);
delay(100);
pitchfn0 = GetPitchMeasurement()-pitchfn;

mcpDac2ASend(pitchXn1);
delay(100);
pitchfn1 = GetPitchMeasurement()-pitchfn;

pitchXn2=pitchXn1-((pitchXn1-pitchXn0)*pitchfn1)/(pitchfn1-pitchfn0);

  Serial.print(pitchXn0);
    Serial.print("  ");
  Serial.println(pitchfn0);

    Serial.print(pitchXn1);
          Serial.print("  ");

    Serial.println(pitchfn1);
        Serial.println(pitchXn2);

        Serial.println();
pitchXn0 = pitchXn1;
pitchXn1 = pitchXn2;

HW_LED2_TOGGLE;

}
  digitalWrite(Application::LED_PIN_2, LOW);    // turn the LED off by making the voltage LOW
  
}

void Application::calibrate_volume()
{


static int16_t volumeXn0 = 0;
static int16_t volumeXn1 = 0;
static int16_t volumeXn2 = 0;
static float q0 = 0;
static long volumefn0 = 0;
static long volumefn1 = 0;
static long volumefn = 0;

    Serial.begin(9600);
    Serial.println("Volume calibration");
    
  InitialiseVolumeMeasurement();
  interrupts();
  mcpDacInit();


volumeXn0 = 0;
volumeXn1 = 4095;

q0 = (16000000/qMeasurement*460765);

volumefn = q0-600;


mcpDac2BSend(volumeXn0);
delay_NOP(44316);//44316=100ms

volumefn0 = GetVolumeMeasurement();

mcpDac2BSend(volumeXn1);

delay_NOP(44316);//44316=100ms
volumefn1 = GetVolumeMeasurement();


Serial.print(volumefn0);
    Serial.print("  ");
  Serial.println(volumefn1);
  
  
while(abs(volumefn0-volumefn1)>10){

mcpDac2BSend(volumeXn0);
delay_NOP(44316);//44316=100ms
volumefn0 = GetVolumeMeasurement()-volumefn;

mcpDac2BSend(volumeXn1);
delay_NOP(44316);//44316=100ms
volumefn1 = GetVolumeMeasurement()-volumefn;

volumeXn2=volumeXn1-((volumeXn1-volumeXn0)*volumefn1)/(volumefn1-volumefn0);

  Serial.print(volumeXn0);
    Serial.print("  ");
  Serial.println(volumefn0);

    Serial.print(volumeXn1);
          Serial.print("  ");

    Serial.println(volumefn1);
        Serial.println(volumeXn2);

        Serial.println();
volumeXn0 = volumeXn1;
volumeXn1 = volumeXn2;
HW_LED2_TOGGLE;

}
  digitalWrite(Application::LED_PIN_2, HIGH);    // turn the LED off by making the voltage LOW
  
}

void Application::hzToAddVal(float hz) {
  setWavetableSampleAdvance((uint16_t)(hz * HZ_ADDVAL_FACTOR));
}

void Application::playNote(float hz, uint16_t milliseconds = 500, uint8_t volume = 255) {
  vScaledVolume = volume;
  hzToAddVal(hz);
  millitimer(milliseconds);
  vScaledVolume = 0;
}

void Application::playStartupSound() {
  playNote(MIDDLE_C, 150, 25);
  playNote(MIDDLE_C * 2, 150, 25);
  playNote(MIDDLE_C * 4, 150, 25);
}

void Application::playCalibratingCountdownSound() {
  for (int i = 0; i < 5; i++) {
    playNote(MIDDLE_C, 500, 25);
    millitimer(150);
  }
  playNote(MIDDLE_C * 2, 1000, 25);
}

void Application::playModeSettingSound() {
  for (int i = 0; i <= _mode; i++) {
    playNote(MIDDLE_C * 2, 200, 25);
    millitimer(100);
  }
}

void Application::delay_NOP(unsigned long time) {
  volatile unsigned long i = 0;
  for (i = 0; i < time; i++) {
      __asm__ __volatile__ ("nop");
  }
}




