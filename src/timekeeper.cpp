#include "timekeeper.h"

#if !(HAS_LORA)
#if (TIME_SYNC_LORASERVER)
#error TIME_SYNC_LORASERVER defined, but device has no LORA configured
#elif (TIME_SYNC_LORAWAN)
#error TIME_SYNC_LORAWAN defined, but device has no LORA configured
#endif
#endif

// Local logging tag
static const char TAG[] = __FILE__;

// symbol to display current time source
// G = GPS / R = RTC / L = LORA / * = no sync / ? = never synced
const char timeSetSymbols[] = {'G', 'R', 'L', '*', '?'};

bool volatile TimePulseTick = false;
timesource_t timeSource = _unsynced;
time_t _COMPILETIME = compileTime(__DATE__);

TaskHandle_t ClockTask = NULL;
hw_timer_t *ppsIRQ = NULL;

#ifdef HAS_IF482
#if (HAS_SDS011)
#error cannot use IF482 together with SDS011 (both use UART#2)
#endif
HardwareSerial IF482(2); // use UART #2 (#1 may be in use for serial GPS)
#endif

Ticker timesyncer;

void setTimeSyncIRQ() { xTaskNotify(irqHandlerTask, TIMESYNC_IRQ, eSetBits); }

void calibrateTime(void) {
  ESP_LOGD(TAG, "[%0.3f] calibrateTime, timeSource == %d", millis() / 1000.0,
           timeSource);
  time_t t = 0;
  uint16_t t_msec = 0;

  // kick off asychronous lora timesync if we have
#if (HAS_LORA) && ((TIME_SYNC_LORASERVER) || (TIME_SYNC_LORAWAN))
  timesync_request();
#endif

  // if no LORA timesource is available, or if we lost time, then fallback to
  // local time source RTS or GPS
  if (((!TIME_SYNC_LORASERVER) && (!TIME_SYNC_LORAWAN)) ||
      (timeSource == _unsynced)) {

// has RTC -> fallback to RTC time
#ifdef HAS_RTC
    t = get_rtctime();
    // set time from RTC - method will check if time is valid
    setMyTime((uint32_t)t, t_msec, _rtc);
#endif

// no RTC -> fallback to GPS time
#if (HAS_GPS)
    t = get_gpstime(&t_msec);
    // set time from GPS - method will check if time is valid
    setMyTime((uint32_t)t, t_msec, _gps);
#endif

  } // fallback

  else

    // no fallback time source available -> we can't set time
    return;

} // calibrateTime()

// set system time (UTC), calibrate RTC and RTC_INT pps
void IRAM_ATTR setMyTime(uint32_t t_sec, uint16_t t_msec,
                         timesource_t mytimesource) {

  struct timeval tv = {0};

  // called with invalid timesource?
  if (mytimesource == _unsynced)
    return;

  // increment t_sec if t_msec > 1000
  time_t time_to_set = (time_t)(t_sec + t_msec / 1000);

  // do we have a valid time?
  if (timeIsValid(time_to_set)) {

    // if we have msec fraction, then wait until top of second with
    // millisecond precision
    if (t_msec % 1000) {
      time_to_set++;
      vTaskDelay(pdMS_TO_TICKS(1000 - t_msec % 1000));
    }

    tv.tv_sec = time_to_set;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);

    ESP_LOGI(TAG, "[%0.3f] UTC time: %d.000 sec", _seconds(), time_to_set);

    // if we have a software pps timer, shift it to top of second
    if (ppsIRQ != NULL) {

      timerWrite(ppsIRQ, 0); // reset pps timer
      CLOCKIRQ();            // fire clock pps, this advances time 1 sec
    }

// if we have got an external timesource, set RTC time and shift RTC_INT pulse
// to top of second
#ifdef HAS_RTC
    if ((mytimesource == _gps) || (mytimesource == _lora))
      set_rtctime(time_to_set);
#endif

    timeSource = mytimesource; // set global variable
    sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);

    timesyncer.attach(TIME_SYNC_INTERVAL * 60, setTimeSyncIRQ);
    ESP_LOGD(TAG, "[%0.3f] Timesync finished, time was set | timesource=%d",
             _seconds(), mytimesource);
  } else {
    timesyncer.attach(TIME_SYNC_INTERVAL_RETRY * 60, setTimeSyncIRQ);
    ESP_LOGV(TAG,
             "[%0.3f] Failed to synchronise time from source %c | unix sec "
             "obtained from source: %d | unix sec at program compilation: %d",
             _seconds(), timeSetSymbols[mytimesource], time_to_set,
             _COMPILETIME);
  }
}

// helper function to setup a pulse per second for time synchronisation
uint8_t timepulse_init() {

// use time pulse from GPS as time base with fixed 1Hz frequency
#ifdef GPS_INT

  // setup external interupt pin for rising edge GPS INT
  pinMode(GPS_INT, INPUT_PULLDOWN);
  // setup external rtc 1Hz clock as pulse per second clock
  ESP_LOGI(TAG, "Timepulse: external (GPS)");
  return 1; // success

// use pulse from on board RTC chip as time base with fixed frequency
#elif defined RTC_INT

  // setup external interupt pin for falling edge RTC INT
  pinMode(RTC_INT, INPUT_PULLUP);

  // setup external rtc 1Hz clock as pulse per second clock
  if (I2C_MUTEX_LOCK()) {
    Rtc.SetSquareWavePinClockFrequency(DS3231SquareWaveClock_1Hz);
    Rtc.SetSquareWavePin(DS3231SquareWavePin_ModeClock);
    I2C_MUTEX_UNLOCK();
    ESP_LOGI(TAG, "Timepulse: external (RTC)");
    return 1; // success
  } else {
    ESP_LOGE(TAG, "RTC initialization error, I2C bus busy");
    return 0; // failure
  }
  return 1; // success

#else
  // use ESP32 hardware timer as time base with adjustable frequency
  ppsIRQ = timerBegin(1, 8000, true);   // set 80 MHz prescaler to 1/10000 sec
  timerAlarmWrite(ppsIRQ, 10000, true); // 1000ms
  ESP_LOGI(TAG, "Timepulse: internal (ESP32 hardware timer)");
  return 1; // success

#endif
} // timepulse_init

void timepulse_start(void) {
#ifdef GPS_INT // start external clock gps pps line
  attachInterrupt(digitalPinToInterrupt(GPS_INT), CLOCKIRQ, RISING);
#elif defined RTC_INT // start external clock rtc
  attachInterrupt(digitalPinToInterrupt(RTC_INT), CLOCKIRQ, FALLING);
#else                 // start internal clock esp32 hardware timer
  timerAttachInterrupt(ppsIRQ, &CLOCKIRQ, true);
  timerAlarmEnable(ppsIRQ);
#endif

  // get time if we don't have one
  if (timeSource != _set)
    setTimeSyncIRQ(); // init systime by RTC or GPS or LORA
  // start cyclic time sync
  timesyncer.attach(TIME_SYNC_INTERVAL * 60, setTimeSyncIRQ);
}

// interrupt service routine triggered by either pps or esp32 hardware timer
void IRAM_ATTR CLOCKIRQ(void) {

  BaseType_t xHigherPriorityTaskWoken = pdFALSE;

  // syncToPPS(); // currently not used

// advance wall clock, if we have
#if (defined HAS_IF482 || defined HAS_DCF77)
  xTaskNotifyFromISR(ClockTask, uint32_t(time(NULL)), eSetBits,
                     &xHigherPriorityTaskWoken);
#endif

// flip time pulse ticker, if needed
#ifdef HAS_DISPLAY
#if (defined GPS_INT || defined RTC_INT)
  TimePulseTick = !TimePulseTick; // flip pulse ticker
#endif
#endif

  // yield only if we should
  if (xHigherPriorityTaskWoken)
    portYIELD_FROM_ISR();
}

// helper function to check plausibility of a given epoch time
bool timeIsValid(time_t const t) {
  // is t a time in the past? we use compile time to guess
  return (t > _COMPILETIME);
}

// helper function to calculate serial transmit time
TickType_t tx_Ticks(uint32_t framesize, unsigned long baud, uint32_t config,
                    int8_t rxPin, int8_t txPins) {

  uint32_t databits = ((config & 0x0c) >> 2) + 5;
  uint32_t stopbits = ((config & 0x20) >> 5) + 1;
  uint32_t txTime = (databits + stopbits + 1) * framesize * 1000.0 / baud;
  // +1 for the startbit

  return round(txTime);
}

#if (defined HAS_IF482 || defined HAS_DCF77)

#if (defined HAS_DCF77 && defined HAS_IF482)
#error You must define at most one of IF482 or DCF77!
#endif

void clock_init(void) {

// setup clock output interface
#ifdef HAS_IF482
  IF482.begin(HAS_IF482);
#elif defined HAS_DCF77
  pinMode(HAS_DCF77, OUTPUT);
#endif

  time_t userUTCTime = time(NULL);

  xTaskCreatePinnedToCore(clock_loop,           // task function
                          "clockloop",          // name of task
                          2048,                 // stack size of task
                          (void *)&userUTCTime, // start time as task parameter
                          4,                    // priority of the task
                          &ClockTask,           // task handle
                          1);                   // CPU core

  _ASSERT(ClockTask != NULL); // has clock task started?
} // clock_init

void clock_loop(void *taskparameter) { // ClockTask

  // caveat: don't use now() in this task, it will cause a race condition
  // due to concurrent access to i2c bus when reading/writing from/to rtc chip!

#ifdef HAS_TWO_LED
  static bool led1_state = false;
#endif
  uint32_t printtime;
  time_t t = *((time_t *)taskparameter); // UTC time seconds
  time_t last_printtime = 0;

#ifdef HAS_DCF77
  uint8_t *DCFpulse;              // pointer on array with DCF pulse bits
  DCFpulse = DCF77_Frame(t + 61); // load first DCF frame before start
#elif defined HAS_IF482
  static TickType_t txDelay = pdMS_TO_TICKS(1000 - IF482_SYNC_FIXUP) -
                              tx_Ticks(IF482_FRAME_SIZE, HAS_IF482);
#endif

  // output the next second's pulse/telegram after pps arrived
  for (;;) {

    // wait for timepulse and store UTC time in seconds got
    xTaskNotifyWait(0x00, ULONG_MAX, &printtime, portMAX_DELAY);
    t = time_t(printtime);

    /*
        // no confident or no recent time -> suppress clock output
        if ((sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) ||
            !(timeIsValid(t)) || (t == last_printtime))
          continue;
    */

    // no confident or no recent time -> suppress clock output
    if (!(timeIsValid(t)) || (t == last_printtime))
      continue;

#if defined HAS_IF482

    // wait until moment to fire. Normally we won't get notified during this
    // timespan, except when next pps pulse arrives while waiting, because pps
    // was adjusted by recent time sync
    if (xTaskNotifyWait(0x00, ULONG_MAX, &printtime, txDelay) == pdTRUE)
      t = time_t(printtime); // new adjusted UTC time seconds

    // send IF482 telegram
    IF482.print(IF482_Frame(t + 2)); // note: telegram is for *next* second

#elif defined HAS_DCF77

    if ((t % 60) == 59)               // is it time to load new frame?
      DCFpulse = DCF77_Frame(t + 61); // generate frame for next minute

    if ((((t + 61) / 60) % 60) ==   // do we still have a recent frame?
        DCFpulse[60])               // (timepulses could be missed!)
      DCF77_Pulse(t + 1, DCFpulse); // then output next second's pulse

      // else we have no recent frame, thus suppressing clock output

#endif

// pps blink on secondary LED if we have one
#ifdef HAS_TWO_LED
    if (led1_state)
      switch_LED1(LED_OFF);
    else
      switch_LED1(LED_ON);
    led1_state = !led1_state;
#endif

    last_printtime = t;

  } // for
} // clock_loop()

#endif // HAS_IF482 || defined HAS_DCF77

// Convert compile date to reference time "in the past"
time_t compileTime(const String compile_date) {

  char s_month[5];
  int year;
  struct tm t = {0};
  static const char month_names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";

  // store compile time once it's calculated
  static time_t secs = -1;

  if (secs == -1) {

    // determine month
    sscanf(compile_date.c_str(), "%s %d %d", s_month, &t.tm_mday, &year);
    t.tm_mon = (strstr(month_names, s_month) - month_names) / 3;
    t.tm_year = year - 1900;

    // convert to secs
    secs = mkgmtime(&t);
  }

  return secs;
}

static bool IsLeapYear(short year) {
  if (year % 4 != 0)
    return false;
  if (year % 100 != 0)
    return true;
  return (year % 400) == 0;
}

// convert UTC tm time to time_t epoch time
time_t mkgmtime(const struct tm *ptm) {

  const int SecondsPerMinute = 60;
  const int SecondsPerHour = 3600;
  const int SecondsPerDay = 86400;
  const int DaysOfMonth[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

  time_t secs = 0;
  // tm_year is years since 1900
  int year = ptm->tm_year + 1900;
  for (int y = 1970; y < year; ++y) {
    secs += (IsLeapYear(y) ? 366 : 365) * SecondsPerDay;
  }
  // tm_mon is month from 0..11
  for (int m = 0; m < ptm->tm_mon; ++m) {
    secs += DaysOfMonth[m] * SecondsPerDay;
    if (m == 1 && IsLeapYear(year))
      secs += SecondsPerDay;
  }
  secs += (ptm->tm_mday - 1) * SecondsPerDay;
  secs += ptm->tm_hour * SecondsPerHour;
  secs += ptm->tm_min * SecondsPerMinute;
  secs += ptm->tm_sec;
  return secs;
}