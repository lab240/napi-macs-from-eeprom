# NAPI EEPROM MAC assignment v11

## История

v11 относительно v10:

* `napi-set-mac` сверен с `NAPI_EEPROM_SPEC.md`: проверка `platform_id`,
  `mac_count <= 8`, CRC до чтения полей; тест на контрольном образе п. 11;
* добавлен `uninstall.sh`;
* спецификация включена в проект.

v10 относительно v9:

Основной механизм: udev-правило `75-napi-mac.rules`, запускающее
`napi-set-mac` на каждое событие `add` физического сетевого интерфейса.
Это закрывает:

* гонку с энумерацией USB при загрузке (`udev-settle` убран);
* повторную энумерацию USB после reset, когда `smsc95xx` выдаёт новый случайный MAC;
* hotplug.

`napi-mac.service` остаётся как страховочный проход и как способ применить
изменённый конфиг без перезагрузки.

Исправления в `napi-set-mac`:

* из EEPROM читаются только 126 байт, а не весь чип;
* USB-порт сравнивается только с собственным каталогом устройства (`1-1.1`),
  адаптер за внешним хабом на этом порту больше не подхватывается;
* проверка уникальности `mac_slot` среди включённых секций;
* логирование в syslog под udev, коды выхода 0 / 3 / 1.

`install.sh` не перезаписывает существующий `mac.conf`.

## Формат EEPROM

Раскладка v3 по `NAPI_EEPROM_SPEC.md` (редакция 2026-09-24). Скрипт использует
только `magic`, `format_version`, `platform_id`, `mac_count`, `mac[0..7]` и
`crc32`; остальные поля не читаются. Проверяется на контрольном образе из п. 11
спецификации (CRC `0xE53A0E41`, MAC1 `02:9e:e6:97:4d:60`).

## Тест

```sh
./install.sh
udevadm trigger --subsystem-match=net --action=add
journalctl -t napi-set-mac --no-pager
ip -br link
reboot
journalctl -b -t napi-set-mac -u napi-mac.service --no-pager
```

Ожидаемое после перезагрузки: строки `napi-set-mac[..]: ethN: usb_eth_M MACK xx:xx:.. OK`
от udev и `... ALREADY` от сервиса.

Проверка USB re-enumeration:

```sh
echo 1-1.1 > /sys/bus/usb/drivers/usb/unbind
echo 1-1.1 > /sys/bus/usb/drivers/usb/bind
journalctl -t napi-set-mac -n 3
```

## Удаление

```sh
./uninstall.sh          # конфиг /etc/napi сохраняется
./uninstall.sh --purge  # вместе с конфигом
```

Уже назначенные MAC остаются на интерфейсах до перезагрузки.
