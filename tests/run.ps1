# 菜单核心回归测试：编译并运行三个主机侧测试，失败时退出码非 0。
#
# 用法：pwsh -File .\run.ps1   # 默认编译 ..\firmware\code\menu 里的核心
#       pwsh -File .\run.ps1 -MenuDir D:\path\menu  # 指定其他菜单核心目录
#
# 只做主机侧编译与运行，不碰固件工程；产物写在 %TEMP%\menu-live-test。
param([string]$MenuDir = "")
if ([string]::IsNullOrEmpty($MenuDir)) { $MenuDir = Join-Path $PSScriptRoot "..\firmware\code\menu" }

$ErrorActionPreference = "Stop"
if (-not (Test-Path (Join-Path $MenuDir "menu.c"))) {
    Write-Error "menu.c not found under: $MenuDir"
    exit 2
}

$out = Join-Path $env:TEMP "menu-live-test"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$flags = @("-std=c99", "-Wall", "-Wextra", "-Wno-unused-parameter", "-pedantic")

# ---- 教程代码与 README 的一致性检查：防止文档里的示例腐烂 ----
$readme = Join-Path $MenuDir "README.md"
$example = Join-Path $PSScriptRoot "realtime_page_example.c"
if ((Test-Path $readme) -and (Test-Path $example)) {
    $md = [System.IO.File]::ReadAllText($readme)
    $sec = [regex]::Match($md, '### 自己做一个实时页[\s\S]*?```c\r?\n([\s\S]*?)```')
    if (-not $sec.Success) {
        Write-Host "WARN  README 里没找到“自己做一个实时页”的示例代码块，跳过一致性检查"
    } else {
        $doc = ($sec.Groups[1].Value -split "\r?\n" | ForEach-Object { $_.TrimEnd() } |
                Where-Object { $_ -ne '' }) -join "`n"
        $src = [System.IO.File]::ReadAllText($example)
        $m = [regex]::Match($src, '/\* ===== README 示例开始 ===== \*/([\s\S]*?)/\* ===== README 示例结束 ===== \*/')
        $code = ($m.Groups[1].Value -split "\r?\n" | ForEach-Object { $_.TrimEnd() } |
                 Where-Object { $_ -ne '' }) -join "`n"
        if ($doc -ne $code) {
            Write-Host "FAIL  README 示例与 realtime_page_example.c 不一致"
            $dl = $doc -split "`n"; $cl = $code -split "`n"
            for ($i = 0; $i -lt [Math]::Max($dl.Count, $cl.Count); ++$i) {
                $a = if ($i -lt $dl.Count) { $dl[$i] } else { '(缺)' }
                $b = if ($i -lt $cl.Count) { $cl[$i] } else { '(缺)' }
                if ($a -ne $b) { Write-Host "  第 $($i+1) 行 README: $a"; Write-Host "  第 $($i+1) 行 示例: $b"; break }
            }
            exit 1
        }
        Write-Host "OK    README 示例与 realtime_page_example.c 一致"
    }
}

Write-Host "== menu core test (sources: $MenuDir) =="
gcc @flags -I"$MenuDir" "$PSScriptRoot\menu_core_test.c" "$MenuDir\menu.c" -o "$out\menu_core_test.exe"
if ($LASTEXITCODE -ne 0) { Write-Error "build failed: menu_core_test"; exit 1 }
& "$out\menu_core_test.exe"
$core = $LASTEXITCODE

Write-Host "== macro form check =="
gcc @flags -I"$MenuDir" "$PSScriptRoot\macro_check.c" -o "$out\macro_check.exe"
if ($LASTEXITCODE -ne 0) { Write-Error "build failed: macro_check"; exit 1 }
& "$out\macro_check.exe"
$macro = $LASTEXITCODE

Write-Host "== realtime page example (README tutorial) =="
gcc @flags -I"$MenuDir" "$PSScriptRoot\realtime_page_example.c" "$MenuDir\menu.c" -o "$out\realtime_page_example.exe"
if ($LASTEXITCODE -ne 0) { Write-Error "build failed: realtime_page_example"; exit 1 }
& "$out\realtime_page_example.exe"
$rt = $LASTEXITCODE

if (($core -ne 0) -or ($macro -ne 0) -or ($rt -ne 0)) {
    Write-Host "FAILED (core=$core macro=$macro realtime=$rt)"
    exit 1
}
Write-Host "ALL GREEN (core=$core macro=$macro realtime=$rt)"
exit 0
