# ESPHome Fine Offset WH1080/WH3080 CC1101 Receiver

ESPHome custom component for receiving **Fine Offset WH1080 / WH3080 compatible weather station sensors** using an **ESP32 and a CC1101 433 MHz transceiver**.

The decoder receives the original OOK weather station transmissions directly and exposes the decoded measurements to ESPHome / Home Assistant.

A key feature of this implementation is proper support for the **87-bit Fine Offset frame variant with a 7-bit preamble**, following the framing behavior documented and implemented by `rtl_433`.

## Features

* ESP32 + CC1101 receiver
* 433.92 MHz OOK/ASK reception
* Fine Offset WH1080 / WH3080 weather protocol
* 87-bit frames with 7-bit preamble
* 88-bit frame support
* CRC-8 validation
* Weather station ID
* Temperature
* Relative humidity
* Wind speed
* Wind gust
* Wind direction
* Cumulative rainfall
* Support for Fine Offset UV/light frame decoding
* ESPHome native sensors
* Home Assistant integration through ESPHome
* Diagnostic logging for RF and protocol debugging

## Why This Project Exists

Fine Offset WH1080/WH3080 weather stations use a simple 433 MHz OOK protocol that is already well understood and supported by `rtl_433`.

However, while developing a direct ESPHome receiver using a CC1101, frames appeared to be received correctly at the RF level but most of them failed CRC validation.

Occasionally, a perfectly valid packet would be received:

```text
RAW: FF A5 02 FE 21 0A 0C 01 37 0E EA
OK weather id=80 Temp: 36.6, Hum: 33, Spd: 12.2, Gust: 14.7, Rain: 93.3, Dir: 315
```

But many other transmissions looked like this:

```text
FF D2 81 7F 90 ...
FF E9 40 BF C8 ...
```

The RF reception itself was not the main problem.

The important detail was **bit alignment**.

## The 87-bit / 7-bit Preamble Problem

The Fine Offset protocol can use a weather frame with a **7-bit preamble**, resulting in an 87-bit transmission instead of the more obvious 88-bit representation.

A decoder that simply searches for an 8-bit `0xFF` preamble and then reads the following bytes can therefore start decoding one bit out of alignment.

This produces data that looks surprisingly close to a valid packet but fails CRC validation.

For example, packets beginning with:

```text
FF D2 81 ...
```

were observed repeatedly while the correctly aligned packets began with:

```text
FF A5 02 ...
```

After changing the decoder to collect the complete RF transmission first and then normalize the bitstream according to its length, the receiver began decoding the 87-bit variant correctly.

Example:

```text
RAW bits=87 pre=7 len=11: FF A5 02 FD 23 09 0B 01 37 0C FA
OK weather bits=87 pre=7 id=80 Temp=36.5 Hum=35 Spd=11.0 Gust=13.5 Rain=93.3 Dir=270
```

The following copy of the transmission was also decoded successfully:

```text
RAW bits=87 pre=7 len=11: FF A5 02 FC 23 0A 0D 01 37 00 23
OK weather bits=87 pre=7 id=80 Temp=36.4 Hum=35 Spd=12.2 Gust=15.9 Rain=93.3 Dir=0
```

CRC errors dropped to zero during testing:

```text
rows=9 len_bad=1 crc_bad=0 hdr_bad=0 ok_w=8 ok_t=0 ok_uv=0
```

## Protocol

The implementation follows the Fine Offset WH1080/WH3080 protocol behavior documented by the `rtl_433` project.

Weather frames contain information including:

```text
Station ID
Temperature
Humidity
Wind speed
Wind gust
Rainfall
Wind direction
CRC
```

The decoder supports both normal and short-preamble framing.

Observed working frames in this setup are primarily:

```text
87 bits
7-bit preamble
11 decoded bytes
```

The normalized weather frame begins with:

```text
FF A...
```

For example:

```text
FF A5 02 F5 23 0B 0F 01 37 0E DE
```

## CRC

Weather frames are validated using the Fine Offset CRC-8:

```text
Polynomial: 0x31
Initial value: 0xFF
```

A complete valid frame produces a zero CRC remainder.

CRC validation is important because 433 MHz receivers will inevitably receive noise and transmissions from unrelated devices.

Only packets passing the protocol checks and CRC validation are published.

## Fine Offset Transmission Behavior

The outdoor sensor does not continuously transmit.

Weather packets are normally transmitted periodically, approximately every 48 seconds for this protocol family.

Multiple closely spaced transmissions may also be observed.

During testing, gaps of approximately 31 ms were visible between related RF activity:

```text
break=30962us
break=30974us
break=32952us
```

This behavior is consistent with the Fine Offset protocol handled by `rtl_433`.

## Hardware

The tested setup uses:

* ESP32
* CC1101 433 MHz module
* External 433 MHz antenna / pigtail
* ESPHome

The CC1101 is configured for approximately:

```text
Frequency:       433.92 MHz
Modulation:      ASK/OOK
Symbol rate:     ~2000 baud
Filter bandwidth: ~203 kHz
```

Example runtime configuration:

```text
CC1101:
  Frequency: 433919840 Hz
  Channel: 0
  Modulation: ASK/OOK
  Symbol Rate: 2002 baud
  Filter Bandwidth: 203125 Hz
```

## Example ESP32 SPI Wiring

The wiring used during development was:

| CC1101     | ESP32  |
| ---------- | ------ |
| SCLK       | GPIO5  |
| MOSI / SDI | GPIO18 |
| MISO / SDO | GPIO13 |
| CS / SS    | GPIO12 |
| VCC        | 3.3 V  |
| GND        | GND    |

**Important:** CC1101 module pinouts are not always identical.

Check the labels and pinout of your specific CC1101 board before connecting it. Some visually similar modules use a different physical pin arrangement.

Do not power a CC1101 module from 5 V unless your specific board explicitly supports it.

## ESPHome Sensors

The example configuration exposes:

* Temperature
* Humidity
* Wind Speed
* Wind Max Speed / Gust
* Wind Direction
* Rain Total
* UV
* Outdoor Illuminance
* Last Station ID

Example Home Assistant entities may therefore look like:

```text
Temperature
Humidity
Wind Speed
Wind max speed
Wind Direction
Rain Total
UV Exterior
Outdoor Illuminance
Last station id
```

## Example Successful Reception

A complete successful reception looks like:

```text
RAW bits=87 pre=7 len=11: FF A5 02 F3 24 07 0A 01 37 0E EA

OK weather bits=87 pre=7
id=80
Temp=35.5
Hum=36
Spd=8.6
Gust=12.2
Rain=93.3
Dir=315
```

ESPHome then publishes changed sensor values:

```text
'Last station id' >> 80
'Temperature' >> 35.5 °C
'Humidity' >> 36 %
'Wind Speed' >> 8.6 km/h
'Wind max speed' >> 12.2 km/h
```

Note that ESPHome may only log sensor publications when their values change.

Therefore, seeing only:

```text
'Last station id' >> 80
```

after an `OK weather` message does **not** mean that the RF packet contained only the station ID.

The complete weather data was decoded; the other sensor values simply had not changed.

## Diagnostics

The component includes diagnostic counters useful while tuning or troubleshooting reception.

Example:

```text
diag edges=1569 valid=784 rows=9 len_bad=1 crc_bad=0 hdr_bad=0 ok_w=8 ok_t=0 ok_uv=0 resets=10 max_bits=87
```

Useful counters include:

* `rows` — completed RF bit rows
* `len_bad` — rows with unsupported lengths
* `crc_bad` — packets rejected by CRC
* `hdr_bad` — invalid protocol headers
* `ok_w` — valid weather packets
* `ok_t` — valid time packets
* `ok_uv` — valid UV/light packets
* `max_bits` — maximum useful frame length observed

For a correctly working WH1080/WH3080 receiver, repeated `ok_w` increments with `crc_bad=0` are a very good sign.

## Files

Typical repository structure:

```text
.
├── README.md
├── LICENSE
├── wh1080.h
└── fineoffset-wh1080.yaml
```

`wh1080.h` contains the Fine Offset decoder.

`fineoffset-wh1080.yaml` contains an example ESPHome configuration for the ESP32 + CC1101 setup.

## Credits

The Fine Offset protocol decoding and framing behavior used by this project is based on the excellent reverse-engineering work in the **rtl_433** project, particularly its Fine Offset WH1080/WH3080 decoder.

rtl_433:

https://github.com/merbanan/rtl_433

Fine Offset WH1080 decoder:

https://github.com/merbanan/rtl_433/blob/master/src/devices/fineoffset_wh1080.c

This project adapts the relevant protocol concepts for direct use with an ESP32, CC1101 and ESPHome.

Please refer to the upstream `rtl_433` project for its licensing terms and original implementation.

## Status

Experimental, but tested successfully with real Fine Offset transmissions.

During initial testing, the original decoder accepted only occasional packets because most transmissions were decoded one bit out of alignment.

With 87-bit / 7-bit preamble handling enabled, repeated weather transmissions were decoded successfully with valid CRC:

```text
crc_bad=0
ok_w=8
```

Contributions and reports from other Fine Offset WH1080/WH3080 owners are welcome.

If you have a compatible station using a different frame length or message type, please open an issue and include DEBUG-level RF logs.
