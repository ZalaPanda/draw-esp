# [ESP32-S3-Touch-AMOLED-2.16](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16)

GitHub [link](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16/blob/main/README.md)

`Get-CimInstance Win32_SerialPort | Select-Object DeviceID, Name` → `COM4`

ESP-IDF Terminal:

```pwsh
PS> & C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
IDF PowerShell Environment
-------------------------
Environment variables set:
IDF_PATH: C:\Util\esp\v6.1\esp-idf
IDF_TOOLS_PATH: C:\Espressif\tools
IDF_PYTHON_ENV_PATH: C:\Espressif\tools\python\v6.1\venv

Custom commands available:
idf.py - Use this to run IDF commands (e.g., idf.py build)
esptool.py
espefuse.py
espsecure.py
otatool.py
parttool.py

PS> idf.py -p COM4 monitor
press Ctrl+T, then Ctrl+R to reset the board
```

`ESP-IDF` `6.1` = Espressif (SDK)  
`esptool` `5.6.0` = ESP32 comm and flash util

## Development

[Arduino](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16/Arduino) or [ESP-IDF](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16/ESP-IDF) development

```bash
esptool --port COM4 --chip esp32s3 flash-id
esptool --port COM4 --chip esp32s3 read-flash 0 ALL factory-backup.bin
espefuse --port COM4 --chip esp32s3 summary
```

Secure Boot: `SECURE_BOOT_EN = False`  
Flash Encryption: `SPI_BOOT_CRYPT_CNT = 0`  
Download mode: `DIS_DOWNLOAD_MODE = False`  
USB Serial/JTAG: `DIS_USB_SERIAL_JTAG = False`  
Secure version: `SECURE_VERSION = 0`  

```pwsh
# get network status
Invoke-RestMethod -Uri "http://192.168.4.1/api/network"

# set AP password
$body = @{ password = "your-ap-password" } | ConvertTo-Json
Invoke-RestMethod -Uri "http://192.168.4.1/api/ap/password" -Method Post -ContentType "application/json" -Body $body

# set STA ssid and password
$body = @{ ssid = "HomeWifi"; password = "your-router-password" } | ConvertTo-Json
Invoke-RestMethod -Uri "http://192.168.4.1/api/wifi" -Method Post -ContentType "application/json" -Body $body

$imagePath = "C:\Users\fuhuaaxh\Downloads\cute.bmp"
curl.exe -i -X POST "http://192.168.0.211/api/image?fit=contain" -H "Content-Type: image/bmp" --data-binary "@$imagePath"
```