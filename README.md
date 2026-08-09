# ESPHome Fine Offset WH1080 / WH3080 CC1101 Receiver

Receive **Fine Offset WH1080 / WH3080 compatible 433 MHz weather stations directly in ESPHome** using an ESP32 and a CC1101 transceiver.

This project was created after finding that a straightforward decoder could occasionally receive a valid packet, but most transmissions failed CRC. The RF signal was actually fine: the important missing detail was support for the **87-bit frame variant with a 7-bit preamble** used by these stations.

The framing and protocol handling are based on the reverse-engineering work in [`rtl_433`](https://github.com/merbanan/rtl_433), especially its Fine Offset WH1080/WH3080 decoder.

## What it provides

- ESP32 + CC1101 433 MHz receiver
- Fine Offset WH1080 / WH3080 weather decoding
- 87-bit frames with 7-bit preamble
- 88-bit frame support
- CRC-8 validation
- Temperature and humidity
- Wind speed and gust
- Wind direction
- Cumulative rainfall
- Station ID
- UV / light frame decoding support
- Native ESPHome sensors for Home Assistant
- RF/protocol diagnostic logging

## Hardware

Tested with:

- ESP32
- CC1101 433 MHz module
- External 433 MHz antenna / pigtail
- ESPHome

### Working setup

![Working ESP32 and CC1101 setup](working%20set.jpg)

### CC1101 module used for testing

Front:

![CC1101 front](CC1101%20front.jpg)

Back:

![CC1101 back](CC1101%20back.jpg)

> **Important:** CC1101 modules that look similar do not always have the same physical pin arrangement. Check the labels on your specific board before connecting it. This was particularly relevant during development of this project.

The module is powered from **3.3 V**.

## Wiring used during development

| CC1101 | ESP32 |
| --- | --- |
| SCLK | GPIO5 |
| MOSI / SDI | GPIO18 |
| MISO / SDO | GPIO13 |
| CS / SS | GPIO12 |
| VCC | 3.3 V |
| GND | GND |

Use the actual labels on your CC1101 board rather than relying only on the physical pin position shown in photographs.

The complete ESPHome configuration, including the CC1101 data/GDO connection used by the decoder, is in [`Fineoffset-WHx080.yaml`](Fineoffset-WHx080.yaml).

## RF configuration

The tested CC1101 configuration is approximately:

```text
Frequency:        433.92 MHz
Modulation:       ASK/OOK
Symbol rate:      ~2000 baud
Filter bandwidth: ~203 kHz
```

Observed at runtime:

```text
Frequency: 433919840 Hz
Channel: 0
Modulation: ASK/OOK
Symbol Rate: 2002 baud
Filter Bandwidth: 203125 Hz
```

## The 87-bit / 7-bit preamble problem

Initially, reception looked unreliable. Occasionally a perfectly valid packet appeared:

```text
RAW: FF A5 02 FE 21 0A 0C 01 37 0E EA
OK weather id=80 Temp: 36.6, Hum: 33, Spd: 12.2, Gust: 14.7, Rain: 93.3, Dir: 315
```

But many transmissions looked like:

```text
FF D2 81 7F 90 ...
FF E9 40 BF C8 ...
```

These were not simply weak or random RF packets. They were a strong indication that the data was being decoded with the wrong **bit alignment**.

Fine Offset transmissions can use a **7-bit preamble**, producing an 87-bit weather frame rather than an 88-bit frame. A decoder that assumes an 8-bit `0xFF` preamble and immediately starts collecting bytes can therefore end up one bit out of alignment.

The solution implemented here is to collect the complete RF transmission first, determine the framing from its length, and normalize the bitstream before decoding and CRC validation.

After doing that, the previously problematic transmissions decode normally:

```text
RAW bits=87 pre=7 len=11: FF A5 02 FD 23 09 0B 01 37 0C FA
OK weather bits=87 pre=7 id=80 Temp=36.5 Hum=35 Spd=11.0 Gust=13.5 Rain=93.3 Dir=270
```

A second closely spaced transmission can also be decoded:

```text
RAW bits=87 pre=7 len=11: FF A5 02 FC 23 0A 0D 01 37 00 23
OK weather bits=87 pre=7 id=80 Temp=36.4 Hum=35 Spd=12.2 Gust=15.9 Rain=93.3 Dir=0
```

During testing the result changed from occasional valid packets and frequent CRC failures to repeated valid packets with:

```text
rows=9 len_bad=1 crc_bad=0 hdr_bad=0 ok_w=8 ok_t=0 ok_uv=0
```

## Weather data

A decoded weather frame provides:

- Station ID
- Temperature
- Relative humidity
- Wind speed
- Wind gust
- Cumulative rainfall
- Wind direction
- CRC

For example:

```text
RAW bits=87 pre=7 len=11: FF A5 02 F3 24 07 0A 01 37 0E EA
OK weather bits=87 pre=7 id=80 Temp=35.5 Hum=36 Spd=8.6 Gust=12.2 Rain=93.3 Dir=315
```

The normalized weather data begins with the expected Fine Offset `FF A...` pattern.

## CRC

Weather frames are validated using CRC-8 with:

```text
Polynomial:    0x31
Initial value: 0xFF
```

Only packets that pass the protocol and CRC checks are published as weather data.

This is particularly useful at 433 MHz, where the receiver may also see noise and unrelated transmitters.

## Transmission behavior

The weather sensor does not transmit continuously. In testing, weather activity followed the expected Fine Offset cadence of approximately **48 seconds**.

Closely spaced RF activity around 31 ms was also observed:

```text
break=30962us
break=30974us
break=32952us
```

The decoder handles each valid transmission independently.

## ESPHome sensors

The example configuration exposes sensors for:

- Temperature
- Humidity
- Wind Speed
- Wind Max Speed / Gust
- Wind Direction
- Rain Total
- UV
- Outdoor Illuminance
- Last Station ID

ESPHome may only print a sensor state publication when that value changes. Therefore this:

```text
OK weather bits=87 pre=7 id=80 Temp=36.4 Hum=35 Spd=12.2 Gust=15.9 Rain=93.3 Dir=0
'Last station id' >> 80
```

does **not** mean that the RF packet contained only the station ID. The complete weather frame was decoded; the other values simply had not changed since the previous publication.

## Diagnostics

The decoder provides diagnostic counters to make RF and framing problems visible:

```text
diag edges=1569 valid=784 rows=9 len_bad=1 crc_bad=0 hdr_bad=0 ok_w=8 ok_t=0 ok_uv=0 resets=10 max_bits=87
```

Important counters include:

| Counter | Meaning |
| --- | --- |
| `rows` | Completed RF bit rows |
| `len_bad` | Rows with unsupported lengths |
| `crc_bad` | Packets rejected by CRC |
| `hdr_bad` | Invalid protocol headers |
| `ok_w` | Valid weather packets |
| `ok_t` | Valid time packets |
| `ok_uv` | Valid UV/light packets |
| `max_bits` | Maximum useful frame length observed |

Repeated increments of `ok_w` with `crc_bad=0` are a good indication that reception and framing are working correctly.

## Installation

The repository contains:

```text
.
├── README.md
├── Fineoffset-WHx080.yaml
├── wh1080.h
├── CC1101 front.jpg
├── CC1101 back.jpg
└── working set.jpg
```

1. Copy [`wh1080.h`](wh1080.h) into the directory containing your ESPHome YAML file.
2. Use [`Fineoffset-WHx080.yaml`](Fineoffset-WHx080.yaml) as a reference for the ESPHome and CC1101 configuration.
3. Adjust Wi-Fi, API/OTA settings and GPIO assignments for your own ESP32 and CC1101 wiring.
4. Compile and flash with ESPHome.
5. Enable DEBUG logging while testing reception.

## Compatibility

This project has been tested with real Fine Offset-compatible transmissions using the hardware shown above.

The implementation is intended for the WH1080 / WH3080 protocol family. Other Fine Offset models may use different framing or message layouts even if they also operate around 433 MHz.

If you test another compatible station, reports and DEBUG logs are welcome.

## Credits

The protocol decoding and framing behavior in this project is based on the excellent reverse-engineering work of the [`rtl_433`](https://github.com/merbanan/rtl_433) project, particularly:

[`src/devices/fineoffset_wh1080.c`](https://github.com/merbanan/rtl_433/blob/master/src/devices/fineoffset_wh1080.c)

This project adapts the relevant Fine Offset protocol handling for direct use with an ESP32, CC1101 and ESPHome.

Please refer to the upstream `rtl_433` project for its original implementation and licensing terms.

## Status

**Working / experimental.**

The receiver is successfully decoding repeated real-world 87-bit / 7-bit-preamble weather transmissions with valid CRC. More testing with other Fine Offset station variants is welcome.
