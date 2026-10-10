set pagination off
set confirm off
set breakpoint pending on
set print pretty off
handle SIGPIPE nostop noprint pass
break esp_systimer_update_counter if counter == &s->counter[0] && counter->base == 0
commands
silent
printf "COUNTER0_ORIGIN value=%llu base=%lld enabled=%d\n", counter->value, counter->base, counter->enabled
disable 1
continue
end
break esp_systimer_load_counter
commands
silent
printf "COUNTER_LOAD unit=%u value=%llu prior=%llu base=%lld enabled=%d\n", index, s->counter[index].toload, s->counter[index].value, s->counter[index].base, s->counter[index].enabled
continue
end
break esp_systimer_xtal_clk_update
commands
silent
printf "XTAL_UPDATE counter0_value=%llu base=%lld enabled=%d\n", ((ESPSysTimerState *)opaque)->counter[0].value, ((ESPSysTimerState *)opaque)->counter[0].base, ((ESPSysTimerState *)opaque)->counter[0].enabled
continue
end
run
