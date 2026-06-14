# Requirements

## Goal

Seeed Studio XIAO ESP32S3で、HomeKitから操作できる照明用スマートリモコンを作る。

まずは1部屋の照明を確実にON/OFF相当で操作できることを優先する。v1はHomeSpanのみでWeb UIを作らない。Web UIはv2要件として後から追加する。

エアコン対応はv1/v2では対象外とし、照明が動いてから追加検討する。

## Target Hardware

- Board: Seeed Studio XIAO ESP32S3, standard edition
- IR transmitter: external transistor/MOSFET driver controlled by ESP32 GPIO
- IR LEDs: multiple LEDs may be connected on the hardware side
- IR LED power: USB 5V
- ESP32 GPIO only controls the transistor/MOSFET gate/base
- IR receiver: 38kHz demodulating receiver module such as VS1838B or TSOP-series

## Initial Pin Assignment

| Purpose | Pin |
| --- | --- |
| IR transmit control | D1 |
| IR receive input | D2 |
| Config/reset button | D3 |
| Status LED | Not required for v1 |

These pins may be changed in `include/config.h`.

## User-Facing Behavior

### HomeKit

- Use HomeSpan for HomeKit integration.
- Expose the device as one HomeKit `Lightbulb` accessory.
- Support ON/OFF only for v1.
- HomeKit ON sends the learned "点灯" IR signal.
- HomeKit OFF sends the learned "常夜灯" IR signal.

The physical light may not map to true power-off. In v1, HomeKit OFF intentionally means "常夜灯".

### Setup And Maintenance

Do not build a custom Web UI for v1. Web UI belongs to v2.

Use HomeSpan-provided provisioning/setup behavior where possible, plus the physical config/reset button and serial monitor output for device-specific maintenance.

Required v1 actions:

- Wi-Fi setup through HomeSpan-supported setup/provisioning flow
- Learn "点灯" IR signal
- Learn "常夜灯" IR signal
- Report whether both required signals are stored through serial logs
- Test-send learned signals through HomeKit ON/OFF after setup
- Reset Wi-Fi/HomeKit/learned IR data through button/serial-supported flows

## IR Learning

- The device must learn IR signals from an existing remote.
- The required learned commands are:
  - `light_on`: original remote's "点灯" button
  - `night_light`: original remote's "常夜灯" button
- Learned data must be stored on the ESP32.
- Learned data must survive reboot and power loss.
- RAW timing storage is acceptable for v1 because it is faster to support unknown lighting remotes.

## Storage

- Store device settings and learned IR data on ESP32 flash.
- Do not commit local Wi-Fi credentials, pairing-specific values, or generated local config.
- Prefer JSON files on LittleFS unless HomeSpan imposes a better storage path for its own pairing data.

## Wi-Fi Provisioning

- Use HomeSpan-supported Wi-Fi provisioning/setup behavior.
- Hard-coded Wi-Fi credentials are not acceptable for the intended public version.
- If Wi-Fi configuration is missing or connection fails repeatedly, the device should enter setup mode.

## Reset

Support both reset paths:

- External button on D3
- Serial/HomeSpan-supported reset command where practical

Button behavior for v1:

- Short press: no required behavior
- Long press: enter setup/reset state

Exact long-press duration can be decided during implementation.

## Public Repository Constraints

The repository is intended to become public after the first usable version works.

Do not commit:

- Wi-Fi SSID/password
- HomeKit pairing secrets
- Generated local storage files
- Board-specific private notes that include household information

## v1 Scope

Included:

- XIAO ESP32S3 PlatformIO configuration
- HomeSpan-based HomeKit Lightbulb accessory
- HomeSpan-supported Wi-Fi provisioning/setup
- Button/serial-driven IR learning and maintenance
- Persistent storage of two learned IR commands
- IR send through external transistor driver
- README setup instructions

Excluded from v1:

- Air conditioner support
- Brightness control
- Color temperature control
- Multiple HomeKit accessories
- Custom Web UI
- MQTT
- Alexa / Google Home
- Home Assistant-specific integration
- Multi-room support

## v1 Implementation Priority

1. Build and flash successfully on XIAO ESP32S3.
2. Learn and save two IR commands.
3. Expose HomeKit Lightbulb via HomeSpan.
4. Map HomeKit ON/OFF to the saved IR commands.
5. Add reset/setup handling.

## v2 Scope

v2 adds a custom Web UI for users who want browser-based setup and maintenance in addition to HomeKit.

Included:

- Web UI for IR learning and maintenance
- Browser-visible stored/missing state for `light_on` and `night_light`
- Test-send controls for learned signals
- Reset controls for learned IR data and supported setup state

Excluded from v2:

- Air conditioner support
- Brightness control
- Color temperature control
- Multiple HomeKit accessories
- MQTT
- Alexa / Google Home
- Home Assistant-specific integration
- Multi-room support
