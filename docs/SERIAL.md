# Серіал-консоль

115200 бод, USB. З Windows: `py -3 -m serial.tools.miniterm COM3 115200`,
з Linux: `picocom -b 115200 /dev/ttyUSB0`. Команда — рядок + Enter.

| Команда | Дія |
|---|---|
| `setwifi <ssid> <пароль>` | зберегти Wi-Fi і перезавантажитись (SSID без пробілів — інакше через портал/Web UI) |
| `settoken <токен>` | зберегти API-токен і перезавантажитись |
| `setpass <пароль>` | задати/змінити пароль адміністратора Web UI |
| `setapi <url>` | опитувати LAN-проксі (`proxy/README.md`) або емулятор замість ukrainealarm.com; без аргументу — назад на офіційний API. Лише через серіал, не через Web UI (SPEC 133). Старе ім'я `setmock` теж працює |
| `forgetwifi` | стерти Wi-Fi → режим налаштування (AP) |
| `show` | що налаштовано (секрети не показуються — лише «configured») |
| `status` | стан: wifi/ntp/тривоги/mute/реле/heap |
| `config` | поточний конфіг JSON (без секретів) |
| `log` | журнал подій |
| `mute` / `unmute` | заглушити / повернути звук |
| `test` | тест реле (короткий пульс, як кнопка TEST) |
| `restart` | перезавантаження |

Тільки dev-збірка:

| Команда | Дія |
|---|---|
| `sim <тип> full\|partial\|none` | інжекція тривоги в обхід API (типи: air_raid, artillery_shelling, urban_fights, chemical, nuclear) |

Секрети ніколи не виводяться в консоль і не потрапляють у журнал.
