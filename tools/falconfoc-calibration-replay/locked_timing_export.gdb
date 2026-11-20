# Use only with the exact locked ELF built with -g and RT02_TIMING_TRACE=ON.
# Load on the workstation; the operator controls any authorized target connection.

define falconfoc-emit-span
    if (unsigned int)($arg2 - $arg1) != 0
        printf "FCT SPAN %s %u %u %u %u\n", $arg0, $arg1, $arg2, $arg3, $arg4
    end
end

define falconfoc-observe-locked-timing
    printf "FCT 1\n"
    if ($xpsr & 511) != 0
        echo Refused: halt at the approved thread-mode observation point.\n
    else
        printf "FCT ENABLED %u\n", adc.trace_cycles != 0
        if adc.trace_cycles != 0
            falconfoc-emit-span "trigger_irq" rt_trace.trigger.start rt_trace.trigger.end rt_trace.trigger.identity rt_trace.trigger.detail
            falconfoc-emit-span "adc_irq" rt_trace.adc.start rt_trace.adc.end rt_trace.adc.identity rt_trace.adc.detail
            falconfoc-emit-span "spi_cs_irq" rt_trace.tmr5[0].start rt_trace.tmr5[0].end rt_trace.tmr5[0].identity rt_trace.tmr5[0].detail
            falconfoc-emit-span "spi_send_irq" rt_trace.tmr5[1].start rt_trace.tmr5[1].end rt_trace.tmr5[1].identity rt_trace.tmr5[1].detail
            falconfoc-emit-span "spi_close_irq" rt_trace.tmr5[2].start rt_trace.tmr5[2].end rt_trace.tmr5[2].identity rt_trace.tmr5[2].detail
            falconfoc-emit-span "spi_rx_irq" rt_trace.spi.start rt_trace.spi.end rt_trace.spi.identity rt_trace.spi.detail
            falconfoc-emit-span "uart_irq" rt_trace.usart.start rt_trace.usart.end rt_trace.usart.identity rt_trace.usart.detail
            falconfoc-emit-span "supervision" rt_trace.supervision.start rt_trace.supervision.end rt_trace.supervision.identity rt_trace.supervision.detail
            falconfoc-emit-span "serial" rt_trace.serial.start rt_trace.serial.end rt_trace.serial.identity rt_trace.serial.detail
            set $fct = &adc.live.transaction.trace
            set $fct_id = adc.live.transaction.slot_id
            if ($fct->points & 1) != 0
                falconfoc-emit-span "adc_trigger" $fct->compare_irq_cycles $fct->trigger_write_return_cycles $fct_id $fct->compare_irq_phase_ticks
            end
            if ($fct->points & 3) == 3
                falconfoc-emit-span "adc_request_to_pcce_proxy" $fct->trigger_proxy_cycles $fct->pcce_irq_cycles $fct_id adc.live.transaction.generation
            end
            if ($fct->points & 2) != 0
                falconfoc-emit-span "pcce_to_ranks" $fct->pcce_irq_cycles $fct->ranks_ready_cycles $fct_id adc.live.transaction.generation
            end
            if ($fct->points & 4) != 0
                set $fct_status = (unsigned int)adc.live.transaction.h0_result
                falconfoc-emit-span "h0" $fct->h0_start_cycles $fct->h0_finish_cycles $fct_id $fct_status
            end
            if ($fct->points & 8) != 0
                falconfoc-emit-span "adapter" $fct->adapter_start_cycles $fct->adapter_return_cycles $fct_id $fct->adapter_result
            end
            falconfoc-emit-span "encoder_decode" encoder.trace.decode_start_cycles encoder.trace.decode_done_cycles encoder.trace.seq encoder.result
            set $fct_status = (unsigned int)encoder.trace.h1_status
            falconfoc-emit-span "h1" encoder.trace.h1_start_cycles encoder.trace.h1_finish_cycles encoder.trace.seq $fct_status
        end
        printf "FCT LANE h0 %u %u %u %u %u\n", runtime.h0.due_count, runtime.h0.missed, runtime.h0.late_count, runtime.h0.budget_count, runtime.h0.delay_us
        printf "FCT LANE h1 %u %u %u %u %u\n", runtime.h1.due_count, runtime.h1.missed, runtime.h1.late_count, runtime.h1.budget_count, runtime.h1.delay_us
        printf "FCT LANE supervision %u %u %u %u %u\n", runtime.supervision.due_count, runtime.supervision.missed, runtime.supervision.late_count, runtime.supervision.budget_count, runtime.supervision.delay_us
        printf "FCT LOCK %u %u %u %u %u\n", runtime.pipeline.mock.last_request.action, runtime.pipeline.mock.output_confirmed, runtime.pipeline.safety.latched_faults, runtime.motion_session, output.result
        printf "FCT END\n"
    end
end