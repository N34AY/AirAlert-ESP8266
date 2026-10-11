# Тестування

## Хостові тести (основні)

```bash
pio test -e native            # всі 100
pio test -e native -f test_engine   # один suite
```

| Suite | Покриває |
|---|---|
| test_matcher | ієрархічний matching (SPEC 135) |
| test_engine | state machine, підтвердження, escalation (SPEC 136) |
| test_pattern | патерни, guard, черга, mute, millis wrap (SPEC 137) |
| test_api | ISO8601, backoff-драбина, stale (SPEC 8-10) |
| test_parser | фікстури API, битий JSON (SPEC 138, 142) |
| test_config | round-trip, валідація, міграція, формат журналу |
| test_notify | сирена-політика, mute-сценарії (SPEC 50-54) |
| test_integration | наскрізні сценарії (SPEC 204) |

## Фікстури та емулятор API

- `test/fixtures/*.json` — зліпки відповідей API, включно з битими.
- `tools/alerts_emulator.py` — динамічний емулятор ukrainealarm.com:

```bash
python3 tools/alerts_emulator.py --port 8787 --log req.jsonl
# керування наживо (curl, браузер або stdin):
curl 'http://HOST:8787/ctl?cmd=start+air_raid+14'     # тривога в області 14
curl 'http://HOST:8787/ctl?cmd=start+chemical+123+14' # громада в області
curl 'http://HOST:8787/ctl?cmd=stop+air_raid+14'
curl 'http://HOST:8787/ctl?cmd=mode+429'              # ok|401|403|429|500|invalid|slow
curl 'http://HOST:8787/ctl?cmd=status'
curl 'http://HOST:8787/log'                           # журнал запитів (NDJSON)
```

- Сценарії з таймлайном: `--scenario tools/scenarios/demo.txt`
  (є demo.txt і failures.txt). Формат: команди + `sleep N`.
- Кожен запит логгується: час, source IP, токен (обрізаний), IMS,
  код відповіді — у консоль і в `--log` JSONL для аналізу.
- Плата ходить в емулятор у збірці `pio run -e nodemcuv2_mock`
  (див. AIRALERT_MOCK_URL у platformio.ini; хост відредагувати під себе).
- Груповий аналіз: емуляторний JSONL + серіал-лог плати зіставляються
  за часом (обидва в UTC).

### Плата → емулятор

`setmock <url>` у серіалі (dev-збірка) перемикає плату на емулятор без
перепрошивки; порожній `setmock` повертає бойовий API. Якщо плата й хост у
різних підмережах (каскад роутерів може губити відповіді — перевірено),
робочий шлях — тунель:

```bash
python3 tools/alerts_emulator.py --port 8787 &
cloudflared tunnel --url http://127.0.0.1:8787
# серіал: setmock https://<random>.trycloudflare.com/v1/alerts/active.json
```

Правила для тестових локацій:
- області/райони/міста — реальні UID з каталогу (14, 31, 67…)
- громади — UID **≥ 10000** (їх немає в офіційному каталозі; менші числа
  колізують з реальними районами інших областей, і matcher чесно не матчить)

> Уроки: 1) емулятор через конвеєр (`… | head`) блокує stdout і відповіді
> зависають — запускайте detached з виводом у файл; 2) LAN-шлях через
> NAT-каскад може пропускати запити й губити відповіді — тунель надійніший.

## На платі (dev-збірка)

Серіал-консоль 115200: `sim air_raid full|partial|none` — інʼєкція snapshot
в обхід API; `test`, `mute`, `status`, `log`. Переходи реле видно як
`[RELAY] ON/OFF`.
