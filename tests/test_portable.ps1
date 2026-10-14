param([Parameter(Mandatory=$true)][string]$Package, [switch]$SkipTester)
$ErrorActionPreference='Stop'
$Package=[IO.Path]::GetFullPath($Package)
$verification=Join-Path $Package 'verification'
$env:Path="$env:WINDIR\System32;$env:WINDIR"
foreach($name in @('QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH','QML_IMPORT_PATH','QML2_IMPORT_PATH','PYTHONHOME','PYTHONPATH')) {
    Remove-Item -LiteralPath ("Env:"+$name) -ErrorAction SilentlyContinue
}
$env:QT_QPA_PLATFORM='offscreen'
$results=@()
function RunFinite([string]$folder,[string]$exe,[string]$arguments) {
    $path=Join-Path (Join-Path $Package $folder) $exe
    $info=[Diagnostics.ProcessStartInfo]::new()
    $info.FileName=$path
    $info.WorkingDirectory=Split-Path $path
    $info.Arguments=$arguments
    $info.UseShellExecute=$false
    $info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true
    $info.RedirectStandardError=$true
    $p=[Diagnostics.Process]::new()
    $p.StartInfo=$info
    try {
        $p.Start() | Out-Null
        $stdout=$p.StandardOutput.ReadToEndAsync()
        $stderr=$p.StandardError.ReadToEndAsync()
        if(-not $p.WaitForExit(10000)) { $p.Kill(); throw "Startup timed out: $exe" }
        $stdout.GetAwaiter().GetResult() | Set-Content -LiteralPath (Join-Path $verification ($folder+'-smoke.stdout.txt')) -Encoding utf8
        $stderr.GetAwaiter().GetResult() | Set-Content -LiteralPath (Join-Path $verification ($folder+'-smoke.stderr.txt')) -Encoding utf8
        if($p.ExitCode -ne 0) { throw "Startup failed: $exe, exit $($p.ExitCode)" }
        return @{application=$folder;exit_code=$p.ExitCode;passed=$true;mode='offscreen startup, no hardware'}
    } finally { $p.Dispose() }
}
$env:KASU_HMI_SMOKE_TEST='1'
$results+=RunFinite 'KASU_PP' 'kasu_pp.exe' '--bcvm-bind-address 127.0.0.1 --bcvm-listen-port 0 --bcvm-peer-address 127.0.0.1 --bcvm-peer-port 0 --debug-stand-socket portable-startup-smoke'
Remove-Item Env:KASU_HMI_SMOKE_TEST
$udp=[Net.Sockets.UdpClient]::new(0)
$port=$udp.Client.LocalEndPoint.Port
$udp.Dispose()
$env:PYRO_STAND_SMOKE_TEST='1'
$results+=RunFinite 'PyroStand' 'pyro_stand.exe' ("--tester udp --bind-ip 127.0.0.1 --board-ip 127.0.0.1 --board-port "+$port+" --debug-socket portable-startup-no-server")
Remove-Item Env:PYRO_STAND_SMOKE_TEST

if (-not $SkipTester) {
$testerPath=Join-Path $Package 'PyroTester\pyro_tester.exe'
$tester=Start-Process -FilePath $testerPath -WorkingDirectory (Split-Path $testerPath) -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $verification 'PyroTester-smoke.stdout.txt') -RedirectStandardError (Join-Path $verification 'PyroTester-smoke.stderr.txt')
try {
    $log=Join-Path $Package 'PyroTester\app.log'
    $ready=$false
    $deadline=[DateTime]::UtcNow.AddSeconds(15)
    while([DateTime]::UtcNow -lt $deadline) {
        if(Test-Path -LiteralPath $log) {
            $content=Get-Content -LiteralPath $log -Raw -Encoding utf8 -ErrorAction SilentlyContinue
            if($content -match '\u041f\u0440\u0438\u043b\u043e\u0436\u0435\u043d\u0438\u0435 \u0437\u0430\u043f\u0443\u0449\u0435\u043d\u043e') { $ready=$true; break }
        }
        if($tester.HasExited) { break }
        Start-Sleep -Milliseconds 250
    }
    if(-not $ready) { throw 'Supplied PyroTester executable did not confirm GUI startup in app.log' }
    $results+=@{application='PyroTester';passed=$true;mode='supplied one-file executable; startup confirmed by its own log; no hardware'}
} finally {
    $children=@(Get-CimInstance Win32_Process -Filter ("ParentProcessId = "+$tester.Id) -ErrorAction SilentlyContinue)
    foreach($child in $children) { Stop-Process -Id $child.ProcessId -Force -ErrorAction SilentlyContinue }
    if(-not $tester.HasExited) { Stop-Process -Id $tester.Id -Force -ErrorAction SilentlyContinue }
    $tester.WaitForExit(3000) | Out-Null
}
}
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $verification 'portable-smoke.json') -Encoding utf8
foreach($relative in @('PyroTester\app.log','KASU_PP\bcvm-exchange.jsonl','KASU_PP\bcvm-debug.jsonl')) {
    $path=Join-Path $Package $relative
    if(Test-Path -LiteralPath $path) {
        if(-not ([IO.Path]::GetFullPath($path)).StartsWith($Package+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe artifact path' }
        Move-Item -LiteralPath $path -Destination (Join-Path $verification ($relative.Replace('\','-')+'.smoke')) -Force
    }
}
Write-Output "Portable startup checks passed for $($results.Count) included applications."
