# Locked Board1 V1. Load the exact ELF. Connect only under the approved bench procedure.
# The commands assume a stopped target. Never run them while power-stage safety is uncertain.

define falconfoc-capture-status
    set $capture_base = (char *)&falconfoc_capture_ram
    set $capture_state = *(unsigned int *)($capture_base + 5480)
    printf "Board1 capture state %u (0 IDLE, 1 ACTIVE, 2 SEALING, 3 READY)\n", $capture_state
end

define falconfoc-capture-arm
    if $argc != 1
        echo Usage: falconfoc-capture-arm NONZERO_UINT32_ID\n
    else
        falconfoc-capture-status
        if $capture_state == 0 && $arg0 > 0 && $arg0 <= 4294967295
            set *(unsigned int *)&falconfoc_capture_request = $arg0
            echo Request stored in recorder SRAM. Resume the target for acquisition.\n
        else
            echo Refused: expected IDLE and a nonzero uint32 capture ID.\n
        end
    end
end

define falconfoc-capture-cancel
    falconfoc-capture-status
    if $capture_state == 1
        set *(unsigned int *)&falconfoc_capture_cancel = *(unsigned int *)($capture_base + 20)
        echo Cancel stored in recorder SRAM. Resume the target to seal the diagnostic image.\n
    else
        echo Refused: only ACTIVE can be cancelled.\n
    end
end

define falconfoc-capture-export
    falconfoc-capture-status
    if $capture_state == 3
        if *(unsigned int *)$capture_base == 0x31434246 && *(unsigned int *)($capture_base + 4) == 1 && *(unsigned int *)($capture_base + 12) == 5480
            dump binary memory board1.bin $capture_base ($capture_base + 5480)
            echo Wrote board1.bin. Verify the immutable raw image with board_capture.py describe.\n
        else
            echo Refused: unexpected recorder header, version, or image size.\n
        end
    else
        echo Refused: wait for READY before exporting.\n
    end
end

document falconfoc-capture-export
Dump the locked Board1 5480-byte READY image. Run from a new output
directory: GDB's dump command replaces an existing board1.bin.
end