# Controls and settings

Use the Stick's buttons to navigate the Pokéwalker screens. The default layout
uses M for Left and R for Right. For Center, press M and R together.

If Center is hard to trigger, increase the M+R window in Stick Settings. This
gives you more time to press the second button. Keep both pressed together for
at least 20 ms.

| Layout | Left | Center | Right | Open Stick Settings |
| --- | --- | --- | --- | --- |
| 2-key M/R, default | M | M+R | R | L |
| 2-key R/M | R | M+R | M | L |
| 3-key L/R | L | M | R | Native Settings → right past Contrast → Center |
| 3-key R/L | R | M | L | Native Settings → right past Contrast → Center |

## Wake the screen

With the screen off, hold M until the screen wakes, roughly half a second in any layout or rotation.
Release all buttons before navigating. L and R work again once the screen is
awake.

Wake uses native Center-hold qualification and its social-offer opportunity,
rather than a separate wall-clock shortcut. An abandoned hold cancels
preparation. IR entry also consumes its gesture through release, preventing
held input from being replayed into the result screen.

## Change settings

In Stick Settings, M selects the next row, R changes its value, and L closes
the menu. Input layout and screen rotation are independent.

Choose Light or Dark appearance and use Test speaker to check audio.
In two-key layouts, M+R window selects 80, 120 or 160 ms between the two presses;
80 ms is the default. Three-key layouts show Center: M instead.
The game's Contrast setting adjusts backlight brightness.

Your layout, rotation and appearance are remembered across restarts, separately
from your Pokémon and game progress. Settings also shows battery voltage and an
estimated charge percentage.

In three-key layouts, native Volume and Contrast retain their original behavior;
the additional selector opens Stick Settings. Holding L remains a direction and
does not later open an overlay. Native time and background work continue while
Stick Settings captures buttons.

Rapid taps are buffered, but stale input is not played into later screens. If
pending transitions exceed 250 ms or fill the bounded queue, the gesture is
canceled until release. Diagnostic builds report these cancellations.

Unplug USB to carry the Stick. It sleeps between game tasks while continuing
to track walking; hold M for half a second when you want to check your Pokémon.
