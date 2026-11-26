#include "os_idle.h"

void os_idle_init(os_idle_t *t, int32_t timeout_s, int64_t now_ms)
{
    t->timeout_s = timeout_s < 0 ? 0 : timeout_s;
    t->last_input_ms = now_ms;
    t->dimmed = false;
}

void os_idle_set_timeout(os_idle_t *t, int32_t timeout_s, int64_t now_ms)
{
    bool dimmed = t->dimmed;
    os_idle_init(t, timeout_s, now_ms);
    t->dimmed = dimmed;
}

bool os_idle_input(os_idle_t *t, int64_t now_ms)
{
    bool was = t->dimmed;
    t->dimmed = false;
    t->last_input_ms = now_ms;
    return was;
}

bool os_idle_poll(os_idle_t *t, int64_t now_ms)
{
    if (t->timeout_s <= 0 || t->dimmed) {
        return false;
    }
    if (now_ms - t->last_input_ms < (int64_t)t->timeout_s * 1000) {
        return false;
    }
    t->dimmed = true;
    return true;
}
