# Original sketch (2024)

[`sobuh_original/sobuh_original.ino`](sobuh_original/sobuh_original.ino) is the Arduino code exactly as it appeared in the appendix of the 2024 lab report (only blank lines were normalized). It is kept here for reference.

The recommended version is [`../sobuh/sobuh.ino`](../sobuh/sobuh.ino). It keeps the same pins, sorting thresholds (3 g / 12 g) and servo angles (90° / 30° / 150°), and fixes the following:

| Original behavior | Improved version |
|:--|:--|
| The calibration factor is saved to EEPROM but **never read back** at start-up (`setCalFactor(1.0)` is always used), so the scale has to be recalibrated through the Serial Monitor **every time it is powered on** | The stored factor is loaded at start-up; calibration runs only if none is stored, or when you send `r` |
| The LCD is initialized **after** calibration, so it stays blank while you calibrate | The LCD starts first and shows step-by-step instructions |
| Weight is read and sorted every **3 s** | Every **0.5 s** (configurable) |
| The servo is commanded on every reading | The servo only moves when the target position changes |
| LCD shows only the weight | LCD shows the weight **and** the category (LIGHT / HEAVY) |
| Magic numbers spread through the code | Pins, thresholds, angles and timing are named constants at the top |
| No data output | Each sorted fruit is printed as a `DATA,` line that the dashboard can read |

The `calibrate()` and `changeSavedCalFactor()` functions in both versions are adapted from the *Calibration* example of the [HX711_ADC](https://github.com/olkal/HX711_ADC) library (MIT, © 2017 Olav Kallhovd).
