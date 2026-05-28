-- Example: PC watch callback + single file write
-- Replace WATCH_PC with an address from your game.

local WATCH_PC = 0x80001000
local out_file = "script-breakpoint-log.txt"

local function write_text(path, text)
    local file, err = io.open(path, "wb")
    if not file then
        error("failed to open file for write: " .. tostring(err))
    end
    file:write(text)
    file:close()
end

local function append_line(path, text)
    local file, err = io.open(path, "ab")
    if not file then
        error("failed to open file for append: " .. tostring(err))
    end
    file:write(text)
    file:close()
end

write_text(out_file, "watching pc " .. string.format("0x%08X", WATCH_PC) .. "\n")

emu.on_breakpoint(WATCH_PC, function(pc)
    local msg = string.format("hit pc=0x%08X\n", pc)
    append_line(out_file, msg)
end)

