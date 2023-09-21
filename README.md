# SNTP Client

A plugin that synchronizes a Wii U's clock with SNTP.

SNTP Client is a Wii U homebrew plugin for the Aroma environment. It allows the console to automatically synchronize it's date and time through the Internet, similar to the feature found on the Nintendo Switch and other modern devices.

## Installation
A Wii U plugin file will be bundled with each release. It should be placed on your SD card, particularly in `wiiu/environments/aroma/plugins`.
* It's important to have the Aroma environment installed for SNTP Client to work. Please visit our [hacking guide](https://wiiu.hacks.guide/) and the [Aroma webpage](https://aroma.foryour.cafe/) if you would like to softmod your Wii U console.

## Usage
If the program is placed correctly on an SD card, SNTP Client will be listed in the Aroma environment's Wii U Plugin System Config Menu.

* If SNTP Client doesn't show up in the Wii U Plugin System Config Menu, confirm you placed the WPS file on your SD card correctly and restart your console.
* `Configuration -> Syncing Enabled`: Enables syncing to the Internet, `false` by default.
* `Configuration -> Timezone`: The timezone to sync from.
* `Configuration -> Receive Notifications`: Shows a notification whenever SNTP Client adjusts the clock, `true` by default.
* `Preview Time`: Lets you preview what the system's clock is currently set to.

As long as syncing is enabled by the user, the clock will sync whenever SNTP Client starts, or when the plugin settings are exited.

**The changes will not be reflected in the HOME Menu and most other applications right away, so the console will need to be rebooted for changes to be completed.**

## Credits
I hope that I am able to express my thanks as much as possible to those who made this repository possible.
* [GaryOderNichts](https://github.com/GaryOderNichts), for writing the network connection code and figuring out how to set the console's date and time through homebrew (so basically all the functionality).
* [Maschell](https://github.com/Maschell), for his work not only with figuring out setting the date and time, but also his work on the Aroma environment.
* [LumaTeam](https://github.com/LumaTeam), for the time syncing code in [Luma3DS](https://github.com/LumaTeam/Luma3DS), which we based our code off of.
* [Lettier](https://github.com/lettier), for his work on [NTP Client](https://github.com/lettier/ntpclient), which in turn led to the code in both Luma3DS and SNTP Client.
