@echo off
echo Starting WSL2 build for BurningDLL...
echo.

REM Try to use Ubuntu first, fallback to default distro
wsl -d Ubuntu bash -lc "cd /mnt/e/burn && export DEBIAN_FRONTEND=noninteractive && sudo apt-get update -qq && sudo apt-get install -y -qq build-essential cmake pkg-config g++ make git && chmod +x build_wsl2.sh install_deps_wsl2.sh && ./build_wsl2.sh Release" 2>nul

if %ERRORLEVEL% NEQ 0 (
    echo Trying default WSL distro...
    wsl bash -lc "cd /mnt/e/burn && export DEBIAN_FRONTEND=noninteractive && sudo apt-get update -qq && sudo apt-get install -y -qq build-essential cmake pkg-config g++ make git && chmod +x build_wsl2.sh && ./build_wsl2.sh Release"
)

echo.
echo Build process completed.
pause
