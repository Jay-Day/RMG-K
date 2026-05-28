-- Smash 64 Damage % Logger
-- Walks the player struct linked list each frame and prints damage % for all active players.
--
-- Player struct layout (node at p_struct_head):
--   +0x00  u32  next node pointer  (0 = end of list)
--   +0x04  u32  player object ptr  (0 = slot empty, skip)
--   +0x2C  u32  damage % as integer (e.g. 50 = 50%)

local frame_count = 0
local P_STRUCT_HEAD = 0x80130D84

local OFF_NEXT = 0x00
local OFF_OBJ  = 0x04
local OFF_DMG  = 0x2C

emu.on_frame(function()
    frame_count = frame_count + 1
    if frame_count % 60 ~= 0 then return end

    local node_addr = memory.read32(P_STRUCT_HEAD)
    local damages = {}
    local player_index = 1

    while node_addr ~= 0 do
        local next_addr = memory.read32(node_addr + OFF_NEXT)
        local obj_addr  = memory.read32(node_addr + OFF_OBJ)

        if obj_addr ~= 0 then
            local dmg = memory.read32(node_addr + OFF_DMG)
            damages[#damages + 1] = string.format("P%d=%d%%", player_index, dmg)
        end

        player_index = player_index + 1
        node_addr = next_addr
    end

    local dmg_str = (#damages > 0) and table.concat(damages, "  ") or "(no active players)"
    print(string.format("frame=%d  %s", frame_count, dmg_str))
end)
