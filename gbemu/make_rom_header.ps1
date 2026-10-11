<#
  make_rom_header.ps1 — превращает .gb-файл в rom.h для скетча gbemu (только Windows, без Python).

  Использование (в папке со скетчем):
      powershell -ExecutionPolicy Bypass -File make_rom_header.ps1 game.gb
      powershell -ExecutionPolicy Bypass -File make_rom_header.ps1 game.gb rom.h    # явно указать выход

  Получится rom.h с массивом ROM_DATA[] (лежит во flash, ОЗУ не тратит).
#>

param(
    [Parameter(Mandatory = $true)][string]$RomPath,
    [string]$OutPath = "rom.h"
)

if (-not (Test-Path $RomPath)) { Write-Host "Файл не найден: $RomPath"; exit 1 }

$bytes = [System.IO.File]::ReadAllBytes((Resolve-Path $RomPath))
$size = $bytes.Length

if ($size -lt 0x150) { Write-Host "Внимание: файл слишком мал ($size байт) — это точно .gb?" }
if ($size -gt 1400 * 1024) { Write-Host ("Внимание: {0:N2} МБ — может не влезть во flash 2 МБ." -f ($size / 1MB)) }

# название из заголовка (0x0134..0x0143)
$title = ""
try {
    $t = $bytes[0x0134..0x0143]
    $sb2 = New-Object System.Text.StringBuilder
    foreach ($b in $t) {
        if ($b -lt 0x20 -or $b -gt 0x7E) { break }     # только печатные символы
        [void]$sb2.Append([char]$b)
    }
    $title = $sb2.ToString().Trim()
} catch { }

# массив должен быть степенью двойки (скетч читает ROM по маске)
if ($size -ne 0 -and ($size -band ($size - 1)) -ne 0) {
    $pad = 1
    while ($pad -lt $size) { $pad = $pad * 2 }
    Write-Host ("Внимание: размер не степень двойки — дополняем до {0} байт (+{1})." -f $pad, ($pad - $size))
    $bytes = $bytes + (New-Object byte[] ($pad - $size))
    $size = $pad
}
if ([string]::IsNullOrWhiteSpace($title)) {
    $title = [System.IO.Path]::GetFileNameWithoutExtension($RomPath)
}

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("/* rom.h — сгенерировано make_rom_header.ps1 из `"$([System.IO.Path]::GetFileName($RomPath))`" */")
[void]$sb.AppendLine("#ifndef ROM_H")
[void]$sb.AppendLine("#define ROM_H")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("#include <stdint.h>")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("#define ROM_NAME `"$title`"")
[void]$sb.AppendLine("static const unsigned int ROM_SIZE = $size;")
[void]$sb.AppendLine("static const uint32_t ROM_MASK = 0x{0:X};" -f ($size - 1))
[void]$sb.AppendLine("static const uint8_t ROM_DATA[$size] = {")

$line = New-Object System.Text.StringBuilder
for ($i = 0; $i -lt $size; $i++) {
    [void]$line.Append("0x{0:X2}," -f $bytes[$i])
    if (($i % 16) -eq 15) {
        [void]$sb.Append("  ").AppendLine($line.ToString())
        [void]$line.Clear() | Out-Null
    }
}
if ($line.Length -gt 0) { [void]$sb.Append("  ").AppendLine($line.ToString()) }

[void]$sb.AppendLine("};")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("#endif /* ROM_H */")

Set-Content -Path $OutPath -Value $sb.ToString() -Encoding ASCII

Write-Host "Готово: $OutPath"
Write-Host ("  игра:   {0}" -f $title)
Write-Host ("  размер: {0} байт ({1:N0} КиБ)" -f $size, ($size / 1KB))
