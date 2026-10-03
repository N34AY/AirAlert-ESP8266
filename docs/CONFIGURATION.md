# Конфігурація

Все редагується у Web UI; це довідка відповідностей.

`/config.json` (LittleFS, атомарний запис tmp→validate→rename):

| Ключ | Default | Межі | Що робить |
|---|---|---|---|
| alerts.poll_sec | 20 | 10–600 | інтервал опитування API (ukrainealarm приймає ~3 запити/хв: <20 с лише додає 401) |
| alerts.stale_sec | 90 | 30–600 | коли дані вважати застарілими |
| alerts.start_conf / end_conf | 1 / 2 | 1–5 / 1–10 | підтверджень на старт/кінець |
| alerts.partial_active/siren/led | true | — | політика часткової тривоги |
| alerts.notify_escalation | true | — | сигнал при посиленні часткової тривоги до повної |
| alerts.notify_additional_location | false | — | сигнал при охопленні ще однієї вибраної локації |
| alerts.reminders_when_stale | false | — | дозволити нагадування за застарілими даними API |
| relay.active_high | true | — | полярність (визначити при commissioning!) |
| relay.max_on_ms | 30000 | 1000–60000 | safety-ліміт безперервного УВІМК |
| relay.test_ms | 600 | 100–10000 | пауза між СТАРТ і СТОП у тест-циклі (фактично ≥ 600) |
| mute.snooze_min | 15 | 1–240 | довгий тиск MUTE |
| mute.all_types | false | — | mute глушить і нові типи загроз |
| startup.cooldown_sec | 300 | — | анти-reboot-loop пауза |
| locations[] | порожньо | 1–16 у робочому режимі | вибрані користувачем локації; без них API не опитується |
| profiles.<type> | SPEC 45 | on/off ≤60с, repeat ≤20 | патерни start/end/reminder + пріоритет |

Валідація — на пристрої (SPEC 155); UI-обмеження лише зручність.
Невідомі ключі ігноруються, відсутні — дефолтяться (міграція вперед, SPEC 90).
Зміна локацій скидає попередній snapshot і HTTP-валідатор та запускає негайне
опитування, тому стан зі старого набору локацій не переноситься на новий.
