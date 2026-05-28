-- Example: per-frame hook + RAM reads + native Lua file append
-- Load via: View -> Scripting Console -> Run Selected

local log_file = "script-frame-log.txt"
local frame_counter = 0

local function append_line(path, text)
    local file, err = io.open(path, "ab")
    if not file then return false, err end
    file:write(text)
    file:close()
    return true
end

emu.on_frame(function()
    frame_counter = frame_counter + 1

    -- Example addresses (adjust per game)
    local sample_u8  = memory.read8(0x80000000)
    local sample_u16 = memory.read16(0x80000002)
    local sample_u32 = memory.read32(0x80000004)

    local line = string.format(
        "frame=%d pc=0x%08X u8=%d u16=%d u32=%u\n",
        frame_counter, emu.get_pc(), sample_u8, sample_u16, sample_u32)

    append_line(log_file, line)
end)

