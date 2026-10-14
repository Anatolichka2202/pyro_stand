param(
    [string]$DestinationRoot = (Join-Path $PSScriptRoot 'build\portable'),
    [string]$KasuBuildDir = 'C:\qt_repos_2\KASU_PP\build\agent-debug',
    [switch]$SkipTester,
    [string]$TesterExe = 'C:\Users\МилТех\Desktop\пиротестер\piro_tester\desctop_app\dist\ТестерЯПС.exe'
)
$ErrorActionPreference = 'Stop'
$standRoot = $PSScriptRoot
$standBuild = Join-Path $standRoot 'build\stand'
$kasuRoot = 'C:\qt_repos_2\KASU_PP'
$testerRoot = 'C:\Users\МилТех\Desktop\пиротестер\piro_tester'
$packageName = 'Stand-Windows-x64-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
$package = [IO.Path]::GetFullPath((Join-Path $DestinationRoot $packageName))
if (Test-Path -LiteralPath $package) { throw "Output exists: $package" }
New-Item -ItemType Directory -Force -Path $package | Out-Null

function CacheValue([string]$build, [string]$name) {
    $line = Select-String -LiteralPath (Join-Path $build 'CMakeCache.txt') -Pattern ('^' + $name + ':[^=]+=(.*)$') | Select-Object -First 1
    if (-not $line) { throw "Missing CMake value $name in $build" }
    return $line.Matches[0].Groups[1].Value
}
function AssertX64([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing executable: $path" }
    $reader = [IO.BinaryReader]::new([IO.File]::OpenRead($path))
    try {
        $reader.BaseStream.Position = 0x3c
        $peOffset = $reader.ReadUInt32()
        $reader.BaseStream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) { throw "Not a Windows x64 executable: $path" }
    } finally { $reader.Dispose() }
}
function DeployQt([string]$build, [string]$exeName, [string]$folder, [string]$configuration) {
    if ((CacheValue $build 'CMAKE_BUILD_TYPE') -ne $configuration) { throw "Wrong configuration in $build" }
    $source = Join-Path $build $exeName
    AssertX64 $source
    $target = Join-Path $package $folder
    New-Item -ItemType Directory -Force -Path $target | Out-Null
    Copy-Item -LiteralPath $source -Destination (Join-Path $target $exeName)
    $qtRoot = [IO.Path]::GetFullPath((Join-Path (CacheValue $build 'Qt6_DIR') '..\..\..'))
    $qtBin = Join-Path $qtRoot 'bin'
    $compilerBin = Split-Path -Parent (CacheValue $build 'CMAKE_CXX_COMPILER')
    $deployTool = Join-Path $qtBin 'windeployqt.exe'
    if (-not (Test-Path -LiteralPath $deployTool)) { throw "Missing $deployTool" }
    $previousPath = $env:Path
    try {
        $env:Path = "$qtBin;$compilerBin;$previousPath"
        & $deployTool ('--' + $configuration.ToLowerInvariant()) --no-translations --no-ffmpeg --no-opengl-sw --dir $target (Join-Path $target $exeName)
        if ($LASTEXITCODE -ne 0) { throw "windeployqt failed for $exeName" }
    } finally { $env:Path = $previousPath }
    foreach ($dll in @('libc++.dll','libunwind.dll','libwinpthread-1.dll','libgcc_s_seh-1.dll','libstdc++-6.dll')) {
        $runtime = Join-Path $compilerBin $dll
        if (Test-Path -LiteralPath $runtime) { Copy-Item -LiteralPath $runtime -Destination $target -Force }
    }
    $offscreen = Join-Path $qtRoot 'plugins\platforms\qoffscreen.dll'
    if (Test-Path -LiteralPath $offscreen) { Copy-Item -LiteralPath $offscreen -Destination (Join-Path $target 'platforms') -Force }
    @('[Paths]', 'Prefix=.', 'Plugins=.') | Set-Content -LiteralPath (Join-Path $target 'qt.conf') -Encoding ascii
}
if ((CacheValue $KasuBuildDir 'KASU_BUILD_BCVM_DEBUG_MODULE') -ne 'ON') { throw 'KASU Debug module must be enabled' }
DeployQt $KasuBuildDir 'kasu_pp.exe' 'KASU_PP' 'Debug'
DeployQt $standBuild 'pyro_stand.exe' 'PyroStand' 'Release'
'Portable preferences in settings/PyroStand/Operator.ini' | Set-Content -LiteralPath (Join-Path $package 'PyroStand\portable.mode') -Encoding ascii

if (-not $SkipTester) {
AssertX64 $TesterExe
New-Item -ItemType Directory -Force -Path (Join-Path $package 'PyroTester') | Out-Null
Copy-Item -LiteralPath $TesterExe -Destination (Join-Path $package 'PyroTester\pyro_tester.exe')
}

@'
@echo off
setlocal
start "" /D "%~dp0KASU_PP" "%~dp0KASU_PP\kasu_pp.exe" --debug-stand-socket kasupp-debug-events-v1 %*
'@ | Set-Content -LiteralPath (Join-Path $package 'START_KASU.cmd') -Encoding ascii
@'
@echo off
setlocal
start "" /D "%~dp0PyroStand" "%~dp0PyroStand\pyro_stand.exe" %*
'@ | Set-Content -LiteralPath (Join-Path $package 'START_PYROSTAND.cmd') -Encoding ascii
if (-not $SkipTester) {
@'
@echo off
setlocal
start "" /D "%~dp0PyroTester" "%~dp0PyroTester\pyro_tester.exe"
'@ | Set-Content -LiteralPath (Join-Path $package 'START_TESTER.cmd') -Encoding ascii
}
@'
@echo off
setlocal
call "%~dp0START_KASU.cmd"
call "%~dp0START_PYROSTAND.cmd" %*
'@ | Set-Content -LiteralPath (Join-Path $package 'START_STAND.cmd') -Encoding ascii
if (-not $SkipTester) {
@'
@echo off
setlocal
set /p "TESTER_COM=NEW tester RS-485 COM port (example COM7): "
if "%TESTER_COM%"=="" exit /b 1
call "%~dp0START_KASU.cmd"
call "%~dp0START_PYROSTAND.cmd" --board-serial "%TESTER_COM%"
call "%~dp0START_TESTER.cmd"
'@ | Set-Content -LiteralPath (Join-Path $package 'START_COMPARE_NEW_BOARD.cmd') -Encoding ascii
}

New-Item -ItemType Directory -Force -Path (Join-Path $package 'docs'),(Join-Path $package 'verification'),(Join-Path $package 'tools') | Out-Null
Copy-Item -LiteralPath (Join-Path $standRoot 'README.md') -Destination (Join-Path $package 'docs\PYROSTAND.md')
Copy-Item -LiteralPath (Join-Path $kasuRoot 'README.md') -Destination (Join-Path $package 'docs\KASU_PP.md')
foreach ($pair in @(@($standBuild,'pyrostand-ctest.log'),@($KasuBuildDir,'kasupp-ctest.log'))) {
    $log = Join-Path $pair[0] 'Testing\Temporary\LastTest.log'
    if (Test-Path -LiteralPath $log) { Copy-Item -LiteralPath $log -Destination (Join-Path (Join-Path $package 'verification') $pair[1]) }
}
@'
$ErrorActionPreference = 'Continue'
Get-NetIPAddress -AddressFamily IPv4 | Select-Object InterfaceAlias,IPAddress,PrefixLength | Format-Table -AutoSize
Get-NetUDPEndpoint -LocalPort 101,5000 -ErrorAction SilentlyContinue | Select-Object LocalAddress,LocalPort,OwningProcess | Format-Table -AutoSize
Write-Host 'BCVM peer default: 192.168.17.246:101; KASU receive address default: 192.168.17.231:101.'
Write-Host 'New tester firmware source: board 192.168.17.144:101; target computer 192.168.17.20:5000.'
Write-Host 'This script changes no network settings.'
Read-Host 'Enter to close'
'@ | Set-Content -LiteralPath (Join-Path $package 'tools\check-network.ps1') -Encoding ascii
@'
@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\check-network.ps1"
'@ | Set-Content -LiteralPath (Join-Path $package 'CHECK_NETWORK.cmd') -Encoding ascii

if (-not $SkipTester) {
$firmware = Join-Path $testerRoot 'GD32F4xx_Firmware_Library\Examples\ENET\piro-tester'
$firmwareTarget = Join-Path $package 'Firmware\NewTester'
New-Item -ItemType Directory -Force -Path $firmwareTarget | Out-Null
foreach ($folder in @('src','inc')) { Copy-Item -LiteralPath (Join-Path $firmware $folder) -Destination $firmwareTarget -Recurse }
Copy-Item -LiteralPath (Join-Path $firmware 'README.md') -Destination $firmwareTarget
New-Item -ItemType Directory -Force -Path (Join-Path $firmwareTarget 'HEX_REFERENCE') | Out-Null
foreach ($name in @('YLS.hex','YLS_BKHO_A.hex','YLS_BKHO_B.hex')) {
    $hex = Join-Path $firmware ('MDK-ARM\output\' + $name)
    if (Test-Path -LiteralPath $hex) { Copy-Item -LiteralPath $hex -Destination (Join-Path $firmwareTarget 'HEX_REFERENCE') }
}
if (Test-Path -LiteralPath (Join-Path $testerRoot 'arduino')) {
    Copy-Item -LiteralPath (Join-Path $testerRoot 'arduino') -Destination (Join-Path $package 'Firmware') -Recurse
}
}

@'
СТЕНД — WINDOWS x64, PORTABLE

Распаковать весь ZIP в доступную для записи папку. Сохранять папки приложений целиком.
Qt и Python отдельно для запуска этих exe не требуются. Аппаратные USB/COM-драйверы
в комплект не входят; COM выбирается по подключённому адаптеру.

START_STAND.cmd — КАСУ ПП Debug + C++ пиростенд.
START_COMPARE_NEW_BOARD.cmd — ввод COM новой платы; C++ читает RS-485, приложение
пиротестера читает Ethernet. В приложении пиротестера нужно включить приём.
START_KASU.cmd, START_PYROSTAND.cmd, START_TESTER.cmd — запуск по отдельности.
CHECK_NETWORK.cmd — просмотр адресов и занятых портов, без изменения сети.
Запускать одну копию КАСУ ПП и один приёмник на каждом выбранном UDP/COM.

ПОДКЛЮЧЕНИЕ
В C++ открыть «Подключение пиротестера», выбрать старую/новую плату и фактические
COM/IP/ЯПС БКД-А-1, БКД-Б и КП8. Внешний JSON настройки не нужен.
Настройки C++ сохраняются в PyroStand/settings/PyroStand/Operator.ini.
Один старый COM: выбор БКД-А-1 или БКД-Б. Два старых COM: первый А-1, второй Б,
КП8 нужен в каждом потоке. Параметры можно передать START_PYROSTAND.cmd:
  --legacy-serial COM7 --legacy-block A1
  --legacy-serial COM7 --legacy-block B
  --legacy-serial COM7 --legacy-second-serial COM8
Для новой Ethernet-платы: --tester udp.
Одновременно новый Ethernet и старый COM: --tester udp --compare-legacy-serial COM9
и --compare-legacy-block A1 либо B по проводке. Отчёты и опоры плат независимы.
Поканальный UI: все назначенные входы, аппаратный тик, факт и дельта.
Расчётный прогноз КП обозначен ≈ и не подтверждает допуск.
Не открывать одновременно два приёмника UDP 5000 на одном ПК.
Если C++ принимает UDP, для приложения пиротестера нужен отдельный RS-485-приём;
его фактический источник проверить в журнале приложения.

СЕТЬ
По исходнику новой платы: 192.168.17.144, кадры направляются на 192.168.17.20:5000,
сброс принимается платой на UDP 101. По умолчанию КАСУ ПП ожидает БЦВМ
192.168.17.246:101, локальный адрес 192.168.17.231:101 при наличии на сетевой карте.
Фактическая сеть, адрес назначения БЦВМ и прошитая версия платы проверяются на стенде.
Параметры запуска не назначают адреса сетевой карте.

ПРОГОН
КАСУ ПП получает и загружает актуальную циклограмму. Пиростенд получает файл через
Debug-сокет, проверяет его и состояния загрузки/разрешения. Начать новый сбор
старой платы либо сбросить новую и дождаться нулевых входов. Физический КП8 задаёт
ноль измерений. Сброс из второго приложения во время прогона не выполнять.
Сохранить отчёт C++ и файл .frames.jsonl, архив КАСУ ПП и результаты пиротестера.
Архивы КАСУ ПП записываются в KASU_PP; журнал Python — в PyroTester/app.log,
результаты Python — в PyroTester/tests_results.

ОГРАНИЧЕНИЯ
Встроено распределение владельца: А-1 пункты 3/5/16, Б пункты 20/21/22,
группы каналов 1,2 / 3,4,5 / 6,7. Назначение действий также проверяется.
Старый пример cyclogram_full.ini содержит FAIRING_SEPARATION в пункте 22.
Этот пример не поставляется как задание для прогона.
В изученном исходнике БЦВМ часть нужных действий пока только пишет журнал.
Установленную на БЦВМ реализацию и физические выходы проверить отдельно.
Этот комплект не является образом БЦВМ и не прошивает оборудование.

ПРОИСХОЖДЕНИЕ
КАСУ ПП: текущая локальная Debug-сборка с модулем.
C++ пиростенд: текущая локальная Release-сборка, включая незакоммиченные исправления.
Приложение пиротестера: готовый exe из desctop_app/dist, не пересобирался.
Соответствие встроенного в него кода текущим Python-исходникам не подтверждено.
Firmware: справочные исходники и имеющиеся HEX из репозитория платы.
Соответствие HEX текущим исходникам и подходящий вариант платы не подтверждены.
Перед прошивкой определить аппаратный вариант; автоматического прошивания нет.
Подробности и SHA-256 приложений — manifest.json; общие суммы — checksums.sha256.
'@ | Set-Content -LiteralPath (Join-Path $package 'README_RU.txt') -Encoding utf8

if ($SkipTester) {
@'
СТЕНД — WINDOWS x64, PORTABLE: КАСУ ПП Debug + C++ Пиростенд
Распаковать весь ZIP. Qt отдельно не нужен. Приложение Егора и прошивки не включены.
START_STAND.cmd — оба приложения; START_KASU.cmd и START_PYROSTAND.cmd — отдельно.
Запускать одну копию КАСУ ПП и один приёмник на каждом выбранном COM/UDP.
Для нового Ethernet + старого COM9:
START_PYROSTAND.cmd --tester udp --compare-legacy-serial COM9 --compare-legacy-block A1
ЯПС новой платы задавать по проводке; 0 в параметрах означает «не назначен».
Текущая проводка: COM9 → ЯПС 0 / БКД-А 0; КП — канал 8 БКД-А.
Новая плата пока без подключённых входов. Профиль А-1 — сопоставление пунктов
3/5/16; его название не задаёт аппаратный адрес БКД.
Плавные часы — индикация ПК; ≈ — прогноз, не подтверждение допуска.
Для прогона нужны загрузка, разрешение, новый сбор и физический КП.
Подробности — docs/PYROSTAND.md; результаты проверок — verification/.
'@ | Set-Content -LiteralPath (Join-Path $package 'README_RU.txt') -Encoding utf8
}

function GitSnapshot([string]$root) {
    $head = (& git -C $root rev-parse HEAD).Trim()
    $status = @(& git -C $root status --porcelain)
    return @{ root=$root; head=$head; dirty=($status.Count -gt 0); status=$status }
}
$testerSnapshot = if ($SkipTester) { @{included=$false;reason='Owner uses existing stand installation'} } else { GitSnapshot $testerRoot }
$manifest = @{
    format='stand.portable.v1'
    created_at=(Get-Date).ToString('o')
    architecture='Windows x64'
    stand=(GitSnapshot $standRoot)
    kasu=(GitSnapshot $kasuRoot)
    tester=$testerSnapshot
    tester_included=(-not $SkipTester)
    tester_binary_source=$TesterExe
    tester_binary_rebuilt=$false
    executables=@()
}
$executables=@('KASU_PP\kasu_pp.exe','PyroStand\pyro_stand.exe')
if (-not $SkipTester) { $executables+='PyroTester\pyro_tester.exe' }
foreach ($relative in $executables) {
    $item = Get-Item -LiteralPath (Join-Path $package $relative)
    $manifest.executables += @{ file=$relative; sha256=(Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash; bytes=$item.Length }
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $package 'manifest.json') -Encoding utf8
& git -C $standRoot diff --binary | Set-Content -LiteralPath (Join-Path $package 'verification\pyrostand-local-changes.patch') -Encoding utf8
Copy-Item -LiteralPath (Join-Path $standRoot 'tests\test_portable.ps1') -Destination (Join-Path $package 'verification\run-portable-smoke.ps1')
$smokeArguments=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $package 'verification\run-portable-smoke.ps1'),'-Package',$package)
if ($SkipTester) { $smokeArguments+='-SkipTester' }
& "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" @smokeArguments
if ($LASTEXITCODE -ne 0) { throw 'Portable startup verification failed; package retained for diagnosis' }
$sums = foreach ($file in (Get-ChildItem -LiteralPath $package -Recurse -File | Sort-Object FullName)) {
    $relative = $file.FullName.Substring($package.Length + 1).Replace('\','/')
    (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $relative
}
$sums | Set-Content -LiteralPath (Join-Path $package 'checksums.sha256') -Encoding ascii
$zip = "$package.zip"
Compress-Archive -LiteralPath $package -DestinationPath $zip -CompressionLevel Optimal
(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + [IO.Path]::GetFileName($zip) | Set-Content -LiteralPath "$zip.sha256" -Encoding ascii
Write-Output $zip
