# Phase 0 — Research (SPEC §195)

Дата: 2026-08-21. Усе нижче звірено запуском на цільовій машині та платі, не з пам'яті.

## Плата

| Параметр | Значення | Як перевірено |
|---|---|---|
| Чіп | ESP8266EX, кварц 26 МГц | `esptool flash-id` |
| Флеш | 4 МБ (mfr 0x5e, dev 0x4016) | `esptool flash-id` |
| USB-UART | CH340 (VID 1A86, PID 7523), COM3 на Windows-хості | Win32_PnPEntity |
| MAC | 98:f4:ab:xx:xx:xx | `esptool read-mac` |
| Стокова прошивка | Espressif AT v1.3.0.0 / SDK 2.0.0 | AT+GMR |
| Бекап флешу | `../stock-backup/nodemcu-stock-4M.bin` + SHA256 | `read-flash 0x0 0x400000` |

## Середовище збірки

- WSL2 Debian, **без USB**: прошивка й монітор ідуть через Windows
  (`py -3 -m esptool`, COM3). Детально: `../../BUILD-CLI.md` §10.2.
- PlatformIO Core 6.1.19, platform **espressif8266@4.2.1** → Arduino core **3.1.2**
  (SDK 2.2.2-dev, lwIP 2.1.3, BearSSL b024386 — з боот-банера).
- Хостові тести: `pio test -e native`, g++ 12 (Debian), Unity.
- Прошивка: `pio run -e nodemcuv2_dev && py -3 -m esptool --port COM3 --baud 921600 write-flash 0x0 <bin>`.

## Флеш-розкладка

`eagle.flash.4m2m.ld`: 1019 КБ скетч + ~2 МБ LittleFS + 16 КБ EEPROM + RF cal.
OTA-бюджет: образ має вміщатися у вільний сектор → практична межа ~500 КБ
(free sketch space ≥ image size). Скелет = 269 КБ (26%), запас є, але Web UI
(gzip PROGMEM) і TLS з'їдять помітну частину. CI має size-gate (SPEC §146).

## Пам'ять (виміряно)

- Скелет: RAM 34.9% (28.6 КБ), heap після boot **51.7 КБ** вільно.
- Порожній скетч з усіма include (WiFiManager+ArduinoJson+BearSSL+WebServer+LittleFS)
  на arduino-cli давав **91% IRAM** — IRAM, а не флеш, буде вузьким місцем.
  Рішення: `BEARSSL_SSL_BASIC` уже в build_flags; уникати `IRAM_ATTR` без потреби.
- BearSSL TLS handshake ≈ 20+ КБ heap. `setBufferSizes(4096, 512)` обов'язково.

## ukrainealarm.com API v3 (заміряно 2026-09-07; alerts.in.ua — до цього)

- Base: `https://api.ukrainealarm.com`, runtime endpoint: `GET /api/v3/alerts`
  (~20 КБ, масив регіонів з активними тривогами), заголовок `Authorization: <key>`
  без схеми. Cloudflare має два ланцюжки — ECDSA (WE1 → GTS Root R4) і RSA
  (WR1 → GTS Root R1) — і обирає за ClientHello. Наш клієнт із
  `BEARSSL_SSL_BASIC` пропонує лише 4 static-RSA AES-CBC сюїти і не вміє
  верифікувати ECDSA, тож отримує RSA-ланцюжок; Root R1 є серед anchors, а
  Cloudflare приймає `TLS_RSA_WITH_AES_128_CBC_SHA(256)` (перевірено openssl
  з обмеженим cipher list + RSA sigalgs). Trust anchors міняти не треба.
- Форма: `[{regionId, regionType, activeAlerts:[{regionId, type,
  activeAlertLevels:[{alertLevel: Red|Yellow, reason, createdAt}]}]}]`. Запис
  регіону містить і успадковані тривоги батьків (район ← область).
- Рівні: `Yellow` = дронова загроза, `Red` = ракетна; обидва можуть бути активні
  одночасно. Є лише для `AIR`; нерівнева тривога має `Red` з порожнім `reason`.
  У прошивці жовтий рівень = окремий тип `air_raid_yellow` з власним профілем.
- Типи: AIR, ARTILLERY, URBAN_FIGHTS, CHEMICAL, NUCLEAR, INFO (INFO — не загроза,
  пропускається).
- **401 = rate-limit** (заміряно 2026-09-07, ~150 запитів, `curl` з WSL2):
  - неправильний ключ → `401`, порожнє тіло, ті самі заголовки, що й у
    «хибних» 401 (`cf-cache-status: DYNAMIC` — відповідь з origin, не Cloudflare);
    без ключа → `403`. Тобто ліміт замаскований під Unauthorized.
  - після 2,5 хв тиші серія 1 запит/с: `ooxoxxxxxxxxxxx` — 3 прийнятих за 4 с,
    далі суцільні 401; наступний прийнятий — через ~60 с після прийнятих (не після
    відхилених: відхилені запити блокування не подовжують).
  - успішність від темпу: 1 с — 20%, 10 с — 28% (5/18), 12 с — 50%, 15 с — 50%
    (6/12), 20 с — 67% (4/6), 30 с — 67–70% (4/6, 7/10), 60 с — 100% (4/4).
    Разом ≈ **3 прийнятих запити/хв на ключ**, вікно ≈ 60 с.
  - `ooxo` на старті і випадкові пропуски на 20–30 с не пояснюються одним
    глобальним лічильником «3 за 60 с» (третій запит мав би пройти) — схоже на
    3 бекенд-інстанси з незалежним лімітом «1 за 60 с» кожен і випадковим
    балансуванням; для прошивки різниці немає.
  - з тиші: keep-alive на одному з'єднанні по 5 с — `oxoxoxxxxxxxo`, нові
    з'єднання по 5 с — `oooxxxxxxxxxx`, 6 паралельних одразу — `oooxxx`: завжди
    рівно 3 прийнятих, наступний — через ~60 с після першого прийнятого.
  - сигнатура клієнта (HTTP/1.0, `User-Agent: ESP8266HTTPClient`,
    `Connection: close`) — та сама успішність (3/4 на 30 с). POP (AMS/CDG) не впливає.
  - висновок для прошивки: 401 — просто «опитати ще раз» у звичайному темпі;
    дефолт 20 с (3/хв ≈ квота, ~2 свіжих знімки/хв, менше змарнованих TLS-
    handshake, ніж на 15 с при тій самій свіжості), stale 90 с; «unauthorized» +
    API_401 і 5-хв auth-backoff SPEC 163 — лише після 15 поспіль.
  - обидва рівні на одній тривозі: червоний домінує (одна фізична тривога = один
    тип); зміна кольору при активному іншому кольорі не озвучує END.
    Квота спільна на ключ: другий споживач того ж ключа вдвічі зменшує свіжість. `Last-Modified`/304 сервер
  не віддає. Каталог: `GET /api/v3/regions` (29 State, 122 District,
  1455 Community; ті самі regionId, що й у alerts.in.ua, крім Криму = 9999),
  генератор `tools/update_locations.py`.
- Не використано: `GET /api/v3/alerts/status` (`lastActionIndex`, 38 байт) —
  кандидат на дешевий change-check замість повного тіла.

## Бібліотеки (pinned)

| Бібліотека | Версія | Причина |
|---|---|---|
| ArduinoJson | ^7.4.3 | stream parsing + filter (SPEC §111) |
| Unity | (test_framework) | host unit tests |

WiFiManager — рішення відкладено до Phase 7: можливий конфлікт з власним Web UI
(SPEC §191); якщо власний captive portal виявиться <200 рядків — обійдемося без нього.

## Рішення

1. **PlatformIO**, не arduino-cli — native tests + pinned deps (SPEC §107).
2. Полярність реле невідома → ACTIVE_HIGH-припущення, `forceRelayOff()` до
   будь-якого init, фізичне підключення сирени тільки в Phase 10 (SPEC §39).
3. Піни: D5=relay, D6=MUTE, D7=TEST, D1/D2=I2C→MCP23017 (SPEC §61).
4. Ядро (lib/core) — чистий C++17 без Arduino, DI-інтерфейси (SPEC §109–110).

## Додаток (Phase 7-8): heap-урок

8 кореневих сертифікатів у X509List на poll-шляху = StoreProhibited під час
TLS handshake (~27 КБ вільного heap не вистачає на парсинг 8 RSA-4096 TA +
handshake). Правило проєкту: **alerts-клієнт тримає рівно 4 GTS-корені**;
широкий бандл (8 CA) — тільки в OTA-режимі, коли все інше зупинено.
Робочий heap після Phase 8: ~27 КБ вільно, TLS-пік залишає ~5 КБ запасу.
