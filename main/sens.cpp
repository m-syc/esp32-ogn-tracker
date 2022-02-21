
#include "hal.h"
#include "sens.h"

#include "timesync.h"

#include "parameters.h"

#include "proc.h"
#include "ctrl.h"
#include "gps.h"
#ifdef WITH_SDLOG
#include "sdlog.h"
#endif

// #define DEBUG_PRINT

#if defined(WITH_BMP180) || defined(WITH_BMP280) || defined(WITH_MS5607) || defined(WITH_BME280) || defined(WITH_MS5611)

#ifdef WITH_BMP180
#include "bmp180.h"
#endif

#ifdef WITH_BMP280
#include "bmp280.h"
#endif

#ifdef WITH_BME280
#include "bme280.h"
#endif

#ifdef WITH_MS5607
#include "ms5607.h"
#endif

#ifdef WITH_MS5611
#include "ms5611.h"
#endif

#include "atmosphere.h"
#include "slope.h"
#include "lowpass2.h"
#include "intmath.h"

#include "fifo.h"

// static const uint8_t  VarioVolume     =    2; // [0..3]
static const uint16_t VarioBasePeriod = 800;  // [ms]

#if defined(WITH_BEEPER) && defined(WITH_VARIO)
void VarioSound(int32_t ClimbRate)
{
  uint8_t VarioVolume = KNOB_Tick>>1; if(VarioVolume>3) VarioVolume=3;  // take vario volume from the user knob
  if(ClimbRate>=50)                                                     // if climb > 1/2 m/s
  { uint8_t Note=(ClimbRate-50)/50;                                     // one semitone per 1/2 m/s
    if(Note>=0x0F) Note=0x0F;                                           // stop at 15 thus 8 m/s
    uint16_t Period=(VarioBasePeriod+(Note>>1))/(1+Note);               // beeping period
    Vario_Period=Period; Vario_Fill=Period>>1;                          // period shorter (faster beeping) with stronger climb
    Vario_Note = (VarioVolume<<6) | (0x10+Note); }                      // note to play: higher for stronger climb
  else if(ClimbRate<=(-100))                                            // if sink > 1 m/s
  { uint8_t Note=(-ClimbRate-100)/100;                                  // one semitone per 1 m/s
    if(Note>=0x0B) Note=0x0B;                                           //
    Vario_Period=VarioBasePeriod; Vario_Fill=VarioBasePeriod;           // continues tone
    Vario_Note = (VarioVolume<<6) | (0x0B-Note); }                      // note to play: lower for stronger sink
  else                                                                  // if climb less than 1/2 m/s or sink less than 1 m/s
  { Vario_Note=0x00; }                                                  // no sound
}
#endif // WITH_BEEPER

#ifdef WITH_BMP180
       BMP180   Baro;                       // BMP180 barometer sensor
#endif

#ifdef WITH_BMP280
       BMP280   Baro;                       // BMP280 barometer sensor
#endif

#ifdef WITH_BME280
       BME280   Baro;                       // BMP280 barometer sensor with humidity sensor
#endif

#ifdef WITH_MS5607
       MS5607   Baro;                       // MS5607 barometer sensor
#endif

#ifdef WITH_MS5611
       MS5611   Baro;                       // MS5611 barometer sensor
#endif

static uint32_t AverPress;                  // [ Pa] summed Pressure over several readouts
static uint8_t  AverCount;                  // [int] number of summed Pressure readouts

static SlopePipe<int32_t>        BaroPipe;  // 4-point slope-fit pipe for pressure
static LowPass2<int32_t,6,4,8>   BaroNoise; // low pass (average) filter for pressure noise

static LowPass2<int64_t,10,9,12> PressAver, // low pass (average) filter for pressure
                                 AltAver;   // low pass (average) filter for GPS altitude

static Delay<int32_t, 8>        PressDelay; // 4-second delay for long-term climb rate

static char Line[96];                       // line to prepare the barometer NMEA sentence

static uint8_t InitBaro(void)
{ Baro.Bus=BARO_I2C;
  uint8_t Err=Baro.CheckID();
  if(Err==0) Err=Baro.ReadCalib();
#ifdef WITH_BMP180
  if(Err==0) Err=Baro.AcquireRawTemperature();
  if(Err==0) { Baro.CalcTemperature(); AverPress=0; AverCount=0; }
#endif
#if defined(WITH_BMP280) || defined(WITH_MS5607) || defined(WITH_BME280) || defined(WITH_MS5611)
  if(Err==0) Err=Baro.Acquire();
  if(Err==0) { Baro.Calculate(); }
#endif
  // if(Err) LED_BAT_On();
  return Err==0 ? Baro.ADDR:0; }

static void ProcBaro(void)
{
    static uint8_t PipeCount=0;

    int16_t Sec  = 10*(TimeSync_Time()%60);                               // [0.1sec]
    uint16_t Phase = TimeSync_msTime();                                   // sync to the GPS PPS
    if(Phase>=500) { Sec+=10; vTaskDelay(1000-Phase); }                   // wait till start of the measuring period
              else { Sec+= 5; vTaskDelay( 500-Phase); }
    if(Sec>=600) Sec-=600;                                                // [0.1sec] pressure measurement time

#ifdef WITH_BMP180
    TickType_t Start=xTaskGetTickCount();
    uint8_t Err=Baro.AcquireRawTemperature();                             // measure temperature
    if(Err==0) { Baro.CalcTemperature(); AverPress=0; AverCount=0; }      // clear the average
          else { PipeCount=0;
	         I2C_Restart(Baro.Bus);
                 vTaskDelay(20);
                 InitBaro(); // try to recover I2C bus and baro
		 return; }

    TickType_t End=Start;
    for(uint8_t Idx=0; Idx<16; Idx++)
    { uint8_t Err=Baro.AcquireRawPressure();                              // take pressure measurement
      if(Err==0) { Baro.CalcPressure(); AverPress+=Baro.Pressure; AverCount++; } // sum-up average pressure
      End=xTaskGetTickCount();
      TickType_t Time = End-Start; if(Time>=200) break; }                   // but no longer than 250ms to fit into 0.5 second slot
    TickType_t MeasTick = Start + (End-Start)/2;
    if(AverCount==0) { PipeCount=0; return ; }                              // and we summed-up some measurements
    AverPress = ( (AverPress<<2) + (AverCount>>1) )/AverCount;              // [0.25Pa]] make the average
#ifdef DEBUG_PRINT
    xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
    Format_String(CONS_UART_Write, "BMP180: ");
    Format_UnsDec(CONS_UART_Write, (AverPress+2)/4, 3, 2);
    Format_String(CONS_UART_Write, "hPa/");
    Format_UnsDec(CONS_UART_Write, (uint16_t)AverCount);
    Format_String(CONS_UART_Write, "\n");
    xSemaphoreGive(CONS_Mutex);
#endif
#endif
#if defined(WITH_BMP280) || defined(WITH_MS5607) || defined(WITH_BME280) || defined(WITH_MS5611)
    TickType_t Start=xTaskGetTickCount();
    uint8_t Err=Baro.Acquire();
    TickType_t End=xTaskGetTickCount();
    TickType_t MeasTick = Start + (End-Start)/2;
    if(Err==0) { Baro.Calculate(); }
          else { PipeCount=0; return; }
    AverPress = Baro.Pressure;
            Err=Baro.Acquire();
    if(Err==0) { Baro.Calculate(); }
          else { PipeCount=0; return; }
    AverPress += Baro.Pressure;
    AverPress/=2;                                                       // [0.25Pa]
#endif
    BaroPipe.Input(AverPress);                                          // [0.25Pa]
    if(PipeCount<255) PipeCount++;                                      // count data going to the slope fitting pipe
    if(PipeCount<4) return;

    BaroPipe.FitSlope();                                             // fit the average and slope from the four most recent pressure points
    int32_t PLR = Atmosphere::PressureLapseRate(((AverPress+2)>>2), Baro.Temperature); // [0.0001m/Pa]
    int32_t ClimbRate = (BaroPipe.Slope*PLR)/800;                    // [1/16Pa/0.5sec] * [0.0001m/Pa] x 800 => [0.01m/sec]

    BaroPipe.CalcNoise();                                            // calculate the noise (average square residue)
    uint32_t Noise=BaroNoise.Process(BaroPipe.Noise);                // pass the noise through the low pass filter
             Noise=(IntSqrt(25*Noise)+64)>>7;                        // [0.1 Pa] noise (RMS) measured on the pressure

    int32_t Pressure=BaroPipe.Aver;                                  // [1/16Pa]
    int32_t StdAltitude = Atmosphere::StdAltitude((Pressure+8)>>4);  // [0.1 m]
    int32_t ClimbRate4sec = ((Pressure-PressDelay.Input(Pressure))*PLR)/3200; // [0.01m/sec] climb rate over 4 sec.
#ifdef WITH_VARIO
    VarioSound(ClimbRate);
    // if(abs(ClimbRate4sec)>50) VarioSound(ClimbRate);
    //                      else VarioSound(2*ClimbRate4sec);
#endif
    Pressure = (Pressure+2)>>2;                                      // [0.25 Pa]
    if( (Phase>=500) && GPS_TimeSinceLock )
    { PressAver.Process(Pressure);                                   // [0.25 Pa] pass pressure through low pass filter
      AltAver.Process(GPS_Altitude);                                 // [0.1 m] pass GPS altitude through same low pass filter
    }
    int32_t PressDiff=Pressure-((PressAver.Out+2048)>>12);           // [0.25 Pa] pressure - <pressure>
    int32_t AltDiff = (PressDiff*(PLR>>4))/250;                      // [0.1 m]
    int32_t Altitude=((AltAver.Out+2048)>>12)+AltDiff;               // [0.1 m]

    uint32_t   Time = TimeSync_Time(MeasTick);              // effective time of the pressure measurement
    uint16_t msTime = TimeSync_msTime(MeasTick);

    uint8_t Frac = Sec%10;                                           // [0.1s]
    // if(Frac==0)
    { // GPS_Position *PosPtr = GPS_getPosition(Sec/10);                // get GPS position record for this second
      uint8_t BestIdx; int16_t BestRes;
      GPS_Position *PosPtr = GPS_getPosition(BestIdx, BestRes, Sec/10, (int16_t)100*Frac, 0);
#ifdef DEBUG_PRINT
      xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
      Format_String(CONS_UART_Write, "ProcBaro: ");
      Format_UnsDec(CONS_UART_Write, (uint16_t)Sec, 3, 1);
      Format_String(CONS_UART_Write, "s -> GPS: ");
      if(PosPtr)
      { Format_UnsDec(CONS_UART_Write, (uint16_t)PosPtr->Sec, 2);
        CONS_UART_Write('.');
        Format_UnsDec(CONS_UART_Write, (uint16_t)PosPtr->mSec, 3);
        CONS_UART_Write('s'); }
      Format_String(CONS_UART_Write, " => ");
      Format_SignDec(CONS_UART_Write, BestRes, 4, 3);
      Format_String(CONS_UART_Write, "s\n");
      xSemaphoreGive(CONS_Mutex);
#endif
      if(PosPtr && abs(BestRes)<=250)                                // if found and small time error
      { PosPtr->Pressure    = Pressure;                              // [0.25Pa]
        PosPtr->StdAltitude = StdAltitude;                           // store standard pressure altitude
        PosPtr->ClimbRate   = ClimbRate/10;                          // [0.1m/s]
        PosPtr->Temperature = Baro.Temperature;                      // and temperature in the GPS record
#ifdef WITH_BME280
        if(Baro.hasHumidity())
        { PosPtr->Humidity = Baro.Humidity;
          PosPtr->hasHum=1; }
#endif
        PosPtr->hasBaro=1; }                                         // tick "hasBaro" flag
    }

    uint8_t Len=0;                                                   // start preparing the barometer NMEA sentence
    Len+=Format_String(Line+Len, "$POGNB,");
    Len+=Format_UnsDec(Line+Len, Time%60, 2);
    Line[Len++]='.';
    Len+=Format_UnsDec(Line+Len, msTime/10, 2);
    // Len+=Format_UnsDec(Line+Len, Sec, 3, 1);                         // [sec] measurement time
    Line[Len++]=',';
    Len+=Format_SignDec(Line+Len, Baro.Temperature, 2, 1);           // [degC] temperature
    Line[Len++]=',';
    Len+=Format_UnsDec(Line+Len, (uint32_t)(10*Pressure+2)>>2, 2, 1); // [Pa] pressure
    Line[Len++]=',';
    Len+=Format_UnsDec(Line+Len, Noise, 2, 1);                       // [Pa] pressure noise
    Line[Len++]=',';
    Len+=Format_SignDec(Line+Len, StdAltitude, 2, 1);                // [m] standard altitude (calc. from pressure)
    Line[Len++]=',';
    Len+=Format_SignDec(Line+Len, Altitude,    2, 1);                // [m] altitude (from cross-calc. with the GPS)
    Line[Len++]=',';
    Len+=Format_SignDec(Line+Len, ClimbRate,   3, 2);                // [m/s] climb rate
    Line[Len++]=',';
#ifdef WITH_BME280
    if(Baro.hasHumidity())
    { Len+=Format_SignDec(Line+Len, Baro.Humidity, 3, 1);            // [%] relative humidity
      Line[Len++]=','; }
#endif
    Len+=NMEA_AppendCheckCRNL(Line, Len);
    if(Parameters.Verbose)
    { xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
      Format_String(CONS_UART_Write, Line, 0, Len);                       // send NMEA sentence to the console (UART1)
      xSemaphoreGive(CONS_Mutex); }
#ifdef WITH_SDLOG
    if(Log_Free()>=128)
    { xSemaphoreTake(Log_Mutex, portMAX_DELAY);
      Format_String(Log_Write, Line, 0, Len);                             // send NMEA sentence to the log file
      xSemaphoreGive(Log_Mutex); }
#endif

    Len=0;                                                           // start preparing the PGRMZ NMEA sentence
    Len+=Format_String(Line+Len, "$PGRMZ,");
    int32_t StdFeet=(StdAltitude*3360+512)>>10;
    Len+=Format_SignDec(Line+Len, StdFeet/10, 1, 0, 1);              // [feet] standard altitude (calc. from pressure)
    Line[Len++]=',';
    Len+=Format_String(Line+Len, "f,");                              // normally f for feet, but metres and m works with XcSoar
    Len+=Format_String(Line+Len, "3");                               // 1 no fix, 2 - 2D, 3 - 3D; assume 3D for now
    Len+=NMEA_AppendCheckCRNL(Line, Len);
    if(Parameters.Verbose)
    { xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
      Format_String(CONS_UART_Write, Line, 0, Len);                           // send NMEA sentence to the console (UART1)
      xSemaphoreGive(CONS_Mutex); }
#ifdef WITH_SDLOG
    if(Log_Free()>=128)
    { xSemaphoreTake(Log_Mutex, portMAX_DELAY);
      Format_String(Log_Write, Line, 0, Len);                             // send NMEA sentence to the log file
      xSemaphoreGive(Log_Mutex); }
#endif

    Len=0;
    Len+=Format_String(Line+Len, "$LK8EX1,");
    Len+=Format_UnsDec(Line+Len, (uint32_t)(Pressure+2)>>2);         // [Pa] pressure
    Line[Len++]=',';
    Len+=Format_SignDec(Line+Len, (StdAltitude+5)/10);               // [m] standard altitude (calc. from pressure)
    Line[Len++]=',';
    Len+=Format_SignDec(Line+Len, ClimbRate);                        // [cm/s] climb rate
    Line[Len++]=',';
    Len+=Format_SignDec(Line+Len, (Baro.Temperature+5)/10);          // [degC] temperature
    Line[Len++]=',';
    Len+=Format_UnsDec(Line+Len, (BatteryVoltage+128)>>8, 4, 3);     // [mV] Battery voltage
    // Len+=Format_String(Line+Len, "999");                          // [%] battery level
    Len+=NMEA_AppendCheckCRNL(Line, Len);
    if(Parameters.Verbose)
    { xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
      Format_String(CONS_UART_Write, Line, 0, Len);                           // send NMEA sentence to the console (UART1)
      xSemaphoreGive(CONS_Mutex); }
#ifdef WITH_SDLOG
    if(Log_Free()>=128)
    { xSemaphoreTake(Log_Mutex, portMAX_DELAY);
      Format_String(Log_Write, Line, 0, Len);                             // send NMEA sentence to the log file
      xSemaphoreGive(Log_Mutex); }
#endif

}

#endif // WITH_BMP180/BMP280/BME280



extern "C"
void vTaskSENS(void* pvParameters)
{ vTaskDelay(20);   // this delay seems to be essential - if you don't wait long enough, the BMP180 won't respond properly.

// #ifdef WITH_BEEPER
//   VarioSound(0);
// #endif

#if defined(WITH_BMP180) || defined(WITH_BMP280) || defined(WITH_MS5607) || defined(WITH_BME280) || defined(WITH_MS5611)
  BaroPipe.Clear  (4*90000);
  BaroNoise.Set(12*16);                // guess the pressure noise level

                                       // initialize the GPS<->Baro correlator
  AltAver.Set(0);                      // [m] Altitude at sea level
  PressAver.Set(4*101300);             // [Pa] Pressure at sea level
  PressDelay.Clear(4*101300);

  uint8_t Detected = InitBaro();
#endif

  xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
  Format_String(CONS_UART_Write, "TaskSENS:");

#ifdef WITH_BMP180
  Format_String(CONS_UART_Write, " BMP180: ");
  if(Detected) { Format_String(CONS_UART_Write, " @"); Format_Hex(CONS_UART_Write, Detected); }
         else  Format_String(CONS_UART_Write, " ?!");
#endif

#ifdef WITH_BMP280
  Format_String(CONS_UART_Write, " BMP280: ");
  if(Detected) { Format_String(CONS_UART_Write, " @"); Format_Hex(CONS_UART_Write, Detected); }
         else  Format_String(CONS_UART_Write, " ?!");
#endif

#ifdef WITH_BME280
  Format_String(CONS_UART_Write, " BME280: ");
  if(Detected) { Format_String(CONS_UART_Write, " @"); Format_Hex(CONS_UART_Write, Detected); }
         else  Format_String(CONS_UART_Write, " ?!");
#endif

#ifdef WITH_MS5607
  Format_String(CONS_UART_Write, " MS5607: ");
  if(Detected) { Format_String(CONS_UART_Write, " @"); Format_Hex(CONS_UART_Write, Detected); }
         else  Format_String(CONS_UART_Write, " ?!");
#endif

#ifdef WITH_MS5611
  Format_String(CONS_UART_Write, " MS5611: ");
  if(Detected) { Format_String(CONS_UART_Write, " @"); Format_Hex(CONS_UART_Write, Detected); }
         else  Format_String(CONS_UART_Write, " ?!");
#endif

  Format_String(CONS_UART_Write, "\n");
  xSemaphoreGive(CONS_Mutex);

  while(1)
  {
#if defined(WITH_BMP180) || defined(WITH_BMP280) || defined(WITH_MS5607) || defined(WITH_BME280) || defined(WITH_MS5611)
    if(PowerMode) ProcBaro();
             else vTaskDelay(100);
#else
    vTaskDelay(1000);
#endif
  }
}

