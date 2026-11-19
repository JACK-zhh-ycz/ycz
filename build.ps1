# ============================================================
#  pptx-studio one-click build
#  Usage:  powershell -ExecutionPolicy RemoteSigned -File build.ps1
#          powershell -ExecutionPolicy RemoteSigned -File build.ps1 -WithExe
#  Output: build\pptx-core.dll     editor kernel (called by Python ctypes)
#          build\plugins\*.dll     sample plugins
#          build\pptx-tool.exe     command line tool (only with -WithExe)
#  NOTE: the CLI is built only on request: some antivirus products
#        quarantine freshly linked unsigned exe files on sight.
# ============================================================
param([switch]$WithExe)
# 编译器定位：优先环境变量 YCZ_GXX / PATH，其次常见安装路径
function Find-Tool([string]$name, [string]$fallback) {
  if ($env:YCZ_GXX -and $name -eq 'g++') { return $env:YCZ_GXX }
  $cmd = Get-Command $name -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  if (Test-Path -LiteralPath $fallback) { return $fallback }
  Write-Output "NOT FOUND: $name  — 安装 MinGW-w64 (x86_64) 并加入 PATH，或设置环境变量 YCZ_GXX / YCZ_RC"
  exit 1
}
$G     = Find-Tool "g++"     "D:\Desktop\code\mingw64\bin\g++.exe"
$RC    = Find-Tool "windres" "D:\Desktop\code\mingw64\bin\windres.exe"
$ROOT  = Split-Path -Parent $MyInvocation.MyCommand.Path
$SRC   = "$ROOT\src"
$OBJ   = "$ROOT\build\obj"
$OUT   = "$ROOT\build"
$FLAGS = @("-std=c++17","-O1","-I$SRC")

New-Item -ItemType Directory -Force -Path $OBJ | Out-Null
New-Item -ItemType Directory -Force -Path "$OUT\plugins" | Out-Null

function Compile-Of([string]$rel) {
  $o = "$OBJ\" + (($rel -split '\\')[-1] -replace '\.cpp$','.o')
  & $G @FLAGS -c "$SRC\$rel" -o $o 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { Write-Output "COMPILE FAIL: $rel"; & $G @FLAGS -c "$SRC\$rel" -o $o; exit 1 }
  return $o
}

# ---------------- shared kernel objects ----------------
$base_srcs = @(
  "base\strutil.cpp","base\inflate.cpp","base\zip.cpp",
  "oxml\xmldom.cpp",
  "core\units.cpp","core\model.cpp","core\preset_geom.cpp",
  "io\pptx_io.cpp",
  "html\html_render.cpp",
  "plugin\plugin_host.cpp"
)
$bobjs = @($base_srcs | ForEach-Object { Compile-Of $_ })

# ---------------- version resources ----------------
# 给每个产物写入公司名/产品名/版本/版权：正常软件都有的信息，缺失反而容易被杀软判为可疑
function Compile-Rc([string]$rel, [string]$name) {
  $r = "$OBJ\$name.res"
  & $RC -I "$SRC\res" "$SRC\res\$rel" -O coff -o $r 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { Write-Output "RC SKIP: $rel"; return @() }
  return @($r)
}
$core_res = Compile-Rc "core.rc" "core"
$tool_res = Compile-Rc "tool.rc" "tool"
$plug_res = Compile-Rc "plugin.rc" "plugin"

# ---------------- command line tool (optional) ----------------
# 默认不生成 exe：杀软对刚链接出来的未签名 exe 最敏感，日常改内核不需要它
if ($WithExe) {
  $mobj = Compile-Of "main.cpp"
  $exe  = "$OUT\pptx-tool.exe"
  if (Test-Path -LiteralPath $exe) { Remove-Item -LiteralPath $exe -Force -ErrorAction SilentlyContinue }
  & $G -std=c++17 -O1 -static ($bobjs + $mobj + $tool_res) -o $exe -lgdiplus 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $exe)) {
    Write-Output "EXE SKIP: pptx-tool.exe locked by scanner (core DLL continues)"
  } else {
    Write-Output "EXE OK: $exe"
  }
} else {
  Write-Output "EXE OFF: pass -WithExe to build the command line tool"
}

# ---------------- editor kernel DLL ----------------
$ui_objs = @()
foreach ($f in @("ui\svg_path.cpp","ui\png_enc.cpp","ui\render_gdiplus.cpp")) { $ui_objs += Compile-Of $f }

$aobj = "$OBJ\pptx_api.obj"
& $G @FLAGS -DPPT_BUILD_DLL -c "$SRC\api\pptx_api.cpp" -o $aobj 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
  Write-Output "COMPILE FAIL: api\pptx_api.cpp"
  & $G @FLAGS -DPPT_BUILD_DLL -c "$SRC\api\pptx_api.cpp" -o $aobj
  exit 1
}

# link MinGW runtime statically, otherwise Python ctypes cannot load the DLL
$COREDLL = "$OUT\pptx-core.dll"
if (Test-Path -LiteralPath $COREDLL) { Remove-Item -LiteralPath $COREDLL -Force }

$dllargs = @()
$dllargs += $bobjs
$dllargs += $ui_objs
$dllargs += $aobj
$dllargs += $core_res
$dllargs += @("-o", $COREDLL, "-lgdiplus", "-lole32", "-luuid", "-lshlwapi", "-lgdi32", "-luser32")
& $G -std=c++17 -O1 -static -static-libgcc -static-libstdc++ -shared @dllargs 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $COREDLL)) {
  Write-Output "LINK FAIL: pptx-core.dll"
  & $G -std=c++17 -O1 -static -static-libgcc -static-libstdc++ -shared @dllargs
  exit 1
}
Write-Output "DLL OK: $COREDLL"

# ---------------- sample plugin DLLs ----------------
$plugins = @(
  @{name="watermark"; src="$ROOT\plugins\watermark\watermark.cpp"},
  @{name="progress";  src="$ROOT\plugins\progress\progress.cpp"},
  @{name="stats";     src="$ROOT\plugins\stats\stats.cpp"},
  @{name="webembed";  src="$ROOT\plugins\webembed\webembed.cpp"}
)
foreach ($p in $plugins) {
  $pdll = "$OUT\plugins\$($p.name).dll"
  & $G -std=c++17 -O1 -shared -DPPT_PLUGIN_EXPORTS -I"$SRC" $p.src $plug_res -o $pdll 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { Write-Output "PLUGIN FAIL: $($p.name)"; exit 1 }
  Write-Output "PLUGIN OK: $pdll"
}
Write-Output "BUILD ALL OK"
Write-Output "Run editor:  python $SRC\pyui\editor.py"
