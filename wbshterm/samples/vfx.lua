-- A demo of what init.lua can do to the terminal's chrome.
--
-- Copy it to %APPDATA%\wbshterm\init.lua (right-click, Edit init.lua…
-- opens the file) and it is picked up within a second. Ctrl+Shift+V
-- pauses and resumes the effect; every colour goes back to the theme
-- when it is off.

local running = true
local clock = 0

local function hsv(hue, saturation, value)
    local sector = math.floor(hue * 6) % 6
    local f = hue * 6 - math.floor(hue * 6)
    local p = value * (1 - saturation)
    local q = value * (1 - f * saturation)
    local t = value * (1 - (1 - f) * saturation)

    local r, g, b
    if sector == 0 then r, g, b = value, t, p
    elseif sector == 1 then r, g, b = q, value, p
    elseif sector == 2 then r, g, b = p, value, t
    elseif sector == 3 then r, g, b = p, q, value
    elseif sector == 4 then r, g, b = t, p, value
    else r, g, b = value, p, q end

    return string.format("#%02X%02X%02X", math.floor(r * 255 + 0.5),
        math.floor(g * 255 + 0.5), math.floor(b * 255 + 0.5))
end

local function channels(color)
    return tonumber(color:sub(2, 3), 16), tonumber(color:sub(4, 5), 16), tonumber(color:sub(6, 7), 16)
end

local function mix(from, to, amount)
    local r1, g1, b1 = channels(from)
    local r2, g2, b2 = channels(to)
    return string.format("#%02X%02X%02X",
        math.floor(r1 + (r2 - r1) * amount + 0.5),
        math.floor(g1 + (g2 - g1) * amount + 0.5),
        math.floor(b1 + (b2 - b1) * amount + 0.5))
end

-- A rainbow cursor, a background that slowly breathes towards a drifting
-- tint, a selection colour opposite the cursor on the wheel, and the
-- bright ANSI colours (the ones `ls` and prompts use) rolling through
-- the spectrum.
wbshterm.on("tick", function(info)
    if not running then return nil end

    clock = info.time
    local theme = info.palette
    local hue = (clock * 0.25) % 1
    local breath = 0.5 + 0.5 * math.sin(clock * 0.8)

    local frame = {
        cursor = hsv(hue, 0.85, 1),
        selection = hsv((hue + 0.5) % 1, 0.5, 0.45),
        background = mix(theme.background, hsv((clock * 0.04) % 1, 0.7, 0.16), breath * 0.6),
        ansi = {},
    }

    for slot = 9, 14 do
        frame.ansi[slot + 1] = hsv(((slot - 9) / 6 + clock * 0.2) % 1, 0.75, 1)
    end

    return frame
end)

-- A scanner running along the right of the status bar, and the clock the
-- effect is driven by. The bar is redrawn every frame while a tick
-- handler is registered, so this moves at the same rate.
local scanner_width = 12

wbshterm.on("status_right", function(info)
    local position = math.floor((0.5 + 0.5 * math.sin(clock * 3)) * (scanner_width - 1) + 0.5)
    local cells = {}
    for i = 0, scanner_width - 1 do
        local distance = math.abs(i - position)
        cells[#cells + 1] = distance == 0 and "#" or distance == 1 and "=" or distance == 2 and "-" or " "
    end

    local tint = running and "green" or "yellow"
    table.insert(info.segments, 1, { label = "vfx", value = "[" .. table.concat(cells) .. "]", tint = tint })
    table.insert(info.segments, 2, { label = "t", value = string.format("%.1fs", clock) })
    return info.segments
end)

wbshterm.on("title", function(info)
    return (running and "~ " or "") .. info.title
end)

wbshterm.bind("ctrl+shift+v", function()
    running = not running
    wbshterm.log(running and "vfx on" or "vfx off")
end)
