@echo off
rem Online play: host a game for one other player (README.md, "Online play").
rem The other player needs your IP address, and UDP port 7777 must reach this computer
rem (forwarded on your router, or through a VPN such as Tailscale or ZeroTier).
cd /d "%~dp0"
start "" RampageRE.exe --host %*
