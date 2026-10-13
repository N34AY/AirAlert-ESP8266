# AirAlert proxy (OpenResty)

Один ключ ukrainealarm на всі пристрої в мережі. nginx сам опитує
`api.ukrainealarm.com`, тримає стан тривог по всій Україні в `lua_shared_dict`
і віддає кожному пристрою лише його регіони — в офіційному форматі
`GET /api/v3/alerts`.

```
ukrainealarm ◄── 1 запит / 20 с (не більше 3/хв) ── nginx: shared dict ◄── пристрої (скільки завгодно)
```

| Що | Як |
|---|---|
| Стан | лише в пам'яті; `reload` його зберігає, після рестарту nginx за ~20 с бере заново |
| Формат | офіційний масив `AlertRegionModel`; без `?regions=` — тіло upstream байт у байт |
| `?regions=14,703` | записи цих регіонів, їхніх предків і нащадків (каталог `/api/v3/regions` раз на добу); поки каталогу немає — весь список |
| `Last-Modified` / 304 | змінюється лише коли змінились дані; пристрій уже вміє 304 |
| Застарілі дані | понад 180 с без успішного знімка → **503** (пристрій показує API_OFFLINE, а не стару картину) |
| Доступ | `Authorization: <токен пристрою>` з `devices.txt`; без заголовка 403, чужий токен 401 — як в upstream |
| Ліміт пристрою | 12 запитів/хв з IP → **429** (503 зарезервовано під «дані застарілі») |
| Вебхуки | `POST :8081/webhook/ukrainealarm`: підпис RSA-SHA256 перевіряється, події старші за 5 хв і повтори відкидаються; подія одразу правує знімок у пам'яті |
| `GET /health` | вік знімка, лічильники upstream і вебхуків, остання помилка |

Ключ ukrainealarm лежить лише на сервері (`/etc/airalert/ukrainealarm.key`) і
пристроям не потрібен.

## Розгортання на centric

Потрібен OpenResty (або nginx з `lua-nginx-module` + `lua-resty-core`),
`lua-resty-http` і `lua-resty-openssl`.

```bash
# OpenResty з офіційного репо: https://openresty.org/en/linux-packages.html
apt-get install -y openresty
opm get ledgetech/lua-resty-http fffonion/lua-resty-openssl

mkdir -p /opt/airalert-proxy /etc/airalert
cp -r proxy/lua /opt/airalert-proxy/
cp proxy/nginx/airalert-proxy.conf /etc/openresty/conf.d/   # або include з http{}
# init_by_lua читає файли під master (root), тож 600 root достатньо
install -m 600 /dev/null /etc/airalert/ukrainealarm.key && editor /etc/airalert/ukrainealarm.key
install -m 600 /dev/null /etc/airalert/devices.txt     && editor /etc/airalert/devices.txt
cp proxy/nginx/webhook-public.pem /etc/airalert/   # ключ ukrainealarm для підпису вебхуків
openresty -t && systemctl reload openresty
curl -s http://127.0.0.1:8080/health
```

`devices.txt` — по токену на рядок, коментар через `#`:

```
k3J9…random…  # сирена, штаб
# Xq81…       # вимкнений пристрій
```

Токен: `openssl rand -hex 16`. Після правки файлу — `systemctl reload openresty`.

Що підлаштувати в `airalert-proxy.conf`: `listen`, `resolver` (якщо назовні DNS
закритий), шлях `lua_ssl_trusted_certificate` (Debian/Ubuntu — як є).
Параметри (`poll_every`, `stale_after`, `max_per_min`) — у таблиці `configure{}`,
значення за замовчуванням — у `lua/airalert/proxy.lua`.

## Пристрій

Серіал (`docs/SERIAL.md`), один раз:

```
settoken <токен пристрою з devices.txt>
setapi http://192.168.0.232:8088/api/v3/alerts   # centric: пристрої 8088, вебхук 8089
```

`setapi` без аргументу — назад на офіційний API. Свої локації пристрій додає
сам (`?regions=…`). По HTTP плата не робить TLS-рукостискання: це ~20 КБ heap
і ~3 с блокування веб-інтерфейсу на кожне опитування менше.

## Тести

```bash
proxy/test/run.sh   # Lua-юніти + e2e: OpenResty у docker проти tools/alerts_emulator.py
```

## Вебхуки

Знімок `/alerts` лишається основою й звіркою: вебхуки губляться (~2 %,
переважно відбої — дані проєкту Vidbiy). Подія несе `activeAlertLevels` =
повний стан регіону для цього типу, тож проксі переписує всі копії цієї тривоги
(запис регіону повторює тривоги предків), а новий регіон бере назву й тип з каталогу.

1. Прокинути назовні **лише** порт вебхука (`8081`), не порт пристроїв.
2. Підписка, одна адреса на ключ:
   `curl -X POST -H "Authorization: $KEY" -H 'Content-Type: application/json' -d '{"webHookUrl":"https://…/webhook/ukrainealarm"}' https://api.ukrainealarm.com/api/v3/webhook`
3. Після першої події — `/health`: `webhooks.ok` росте, `webhook_age_s` малий.
   Тоді `poll_every` можна підняти до 60 с.

`webhook-public.pem` — копія `api.ukrainealarm.com/webhook-public.pem`
(сторінку закриває Cloudflare, `curl` її не дістане).
