# MEMS Rain Gauge  
<img src="images/1.JPG"  width="800">
<img src="images/2.png"  width="800">

## Prerequisite
 - ADXL355 ([EVAL-ADXL355-PMDZ](https://www.analog.com/en/products/adxl355.html#product-overview))
 - [M5Stack Tough](https://docs.m5stack.com/en/core/tough)
 - [hat](https://github.com/T40O0/ADXL355_SPI_M5_SD_FIR/blob/main/3D_model/hat.stl)
 - [modified version](https://github.com/T40O0/M5_ADXL355/tree/M5) of [plasmapper/adxl355-arduino](https://github.com/plasmapper/adxl355-arduino)  
Hats off to PL.

## Feaes
 - Cost-effective: It can be built for around €100.
 - Tracking: Records the total acceleration and the number of times acceleration exceeds four preset thresholds.
 - Rainfall conversion: Multiplies the exceedance frequency by a specific coefficient to estimate rainfall (mm).
 - Local storage: Saves data files to a TF (microSD) card.
 - Maintenance-free: Prevents clogging issues caused by volcanic ash, which commonly affect traditional tipping bucket rain gauges.

## Installation & Getting Started
 - Visit the project at [MEMS Seismometer](https://github.com/T40O0/ADXL355_SPI_M5_SD_FIR)

## Note
 - In my testing area, rainfall estimates using the following coefficients gave R<sup>2</sup> = 0.97 with hourly rainfall (mm/h) from a tipping bucket rain gauge located 20 meters away.
   - 5 gal exceedance frequency x 0.0002281 (mm)
   - 10 gal exceedance frequency x 0.0004399 (mm)
 - Data are recorded every minute. These can be aggregated to estimate the rainfall every 10 minutes, etc.
 - A 1000 Hz high-cut filter is applied to the 4000 Hz output data from the ADXL355. The data are re-sampled at 100 Hz without a decimation filter, so frequency analysis is not possible. If you are interested in frequencies, click here [MEMS Seismometer](https://github.com/T40O0/ADXL355_SPI_M5_SD_FIR).
 - A "[hat](https://github.com/T40O0/ADXL355_SPI_M5_SD_FIR/blob/main/3D_model/hat.stl)" is required.
   - [x] Blocks sunlight and protects the LCD.
   - [x] Falling volcanic ash is washed away by rain.
 - Do not submerge the case in water or other liquids.
 - For more information, see [MEMS Seismometer](https://github.com/T40O0/ADXL355_SPI_M5_SD_FIR).

## Licence
This project is licensed under the MIT licence - see the [LICENSE file](LICENSE) for details.
