param(
    [Parameter(Mandatory=$true)][string]$GameRoot,
    [string]$Rom = '', [string]$Framework = '', [string]$Ui = '',
    [string]$Oracle = '', [string]$Build = 'build',
    [string]$Toolchain = 'C:\msys64\mingw64\bin', [string]$Python = '',
    [int]$Jobs = 4, [switch]$NoUi, [switch]$FastBuild,
    [switch]$Production, [switch]$SkipRegenerate
)
$ErrorActionPreference = 'Stop'
if ($Jobs -lt 1) { throw 'Jobs must be positive.' }
if ($FastBuild -and $Production) { throw 'FastBuild is for development; omit it for Production.' }
$GameRoot = (Resolve-Path -LiteralPath $GameRoot).Path
$metadata = Get-Content -LiteralPath (Join-Path $GameRoot 'game-build.json') -Raw | ConvertFrom-Json
if ($metadata.module -notmatch '^[A-Za-z_][A-Za-z_0-9]*$' -or $metadata.sha256 -notmatch '^[0-9a-f]{64}$') {
    throw 'Invalid game-build.json module or ROM identity.'
}
if (!$Framework) { $Framework = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')) }
$Framework = (Resolve-Path -LiteralPath $Framework).Path
if (!$Ui) { $Ui = Join-Path $GameRoot 'recomp-ui' }
if (!$Rom) { $Rom = Join-Path $GameRoot ('roms/' + $metadata.module + '.vb') }
$Rom = (Resolve-Path -LiteralPath $Rom).Path
if ((Get-FileHash -LiteralPath $Rom -Algorithm SHA256).Hash.ToLowerInvariant() -ne $metadata.sha256) {
    throw ('Unsupported cartridge revision for ' + $metadata.module)
}
if (!$Python) {
    $launcher = Get-Command py.exe -ErrorAction SilentlyContinue
    if ($launcher) {
        $candidate = & $launcher.Source -3 -c 'import sys; print(sys.executable)'
        if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $candidate)) { $Python = $candidate }
    }
    if (!$Python) {
        $candidate = Get-Command python.exe -All -ErrorAction SilentlyContinue |
            Where-Object { $_.Source -notmatch '\\usr\\bin\\|\\WindowsApps\\' } | Select-Object -First 1
        if ($candidate) { $Python = $candidate.Source }
    }
}
if (!$Python) { throw 'Pass -Python with a native Windows Python 3.11+ interpreter.' }
if (!(Test-Path -LiteralPath $Python)) { $Python = (Get-Command $Python -ErrorAction Stop).Source }
$Python = (Resolve-Path -LiteralPath $Python).Path
& $Python -c "import sys; assert sys.platform == 'win32' and sys.version_info >= (3,11)"
if ($LASTEXITCODE -ne 0) { throw 'Native Windows Python 3.11+ required.' }
$buildPath = if ([IO.Path]::IsPathRooted($Build)) { [IO.Path]::GetFullPath($Build) } else { [IO.Path]::GetFullPath((Join-Path $GameRoot $Build)) }
foreach ($name in @('cmake.exe','ninja.exe','gcc.exe','g++.exe','ctest.exe','SDL2.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $Toolchain $name))) { throw "Missing build dependency: $Toolchain/$name" }
}
if (!$NoUi -and !(Test-Path -LiteralPath (Join-Path $Ui 'recomp_ui.cmake'))) {
    throw 'Initialize recomp-ui or pass -Ui with its checkout.'
}
if ($FastBuild -and !$metadata.fast_build_option) { throw 'This game has no FastBuild configuration.' }
function Invoke-VbBuildTool([string]$Executable, [string[]]$Arguments) {
    $savedPreference = $ErrorActionPreference
    try { $ErrorActionPreference = 'Continue'; & $Executable @Arguments; $code = $LASTEXITCODE }
    finally { $ErrorActionPreference = $savedPreference }
    if ($code -ne 0) { throw "$Executable failed ($code)" }
}
$savedPath = $env:PATH
try {
    $env:PATH = $Toolchain + ';' + $env:PATH
    if (!$SkipRegenerate) {
        Push-Location -LiteralPath $Framework
        try {
            Invoke-VbBuildTool $Python @('-u','-m','recompiler.cli.vbrecomp_codegen','--rom',$Rom,
                '--module',$metadata.module,'--out',(Join-Path $GameRoot 'generated'),
                '--seeds-toml',(Join-Path $GameRoot ($metadata.module + '.toml')))
        } finally { Pop-Location }
    }
    $options = @('-S',$GameRoot,'-B',$buildPath,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_MAKE_PROGRAM=$Toolchain/ninja.exe","-DCMAKE_C_COMPILER=$Toolchain/gcc.exe",
        "-DCMAKE_CXX_COMPILER=$Toolchain/g++.exe","-DPython3_EXECUTABLE=$Python",
        "-DVBRECOMP_ROOT=$Framework","-DRECOMP_UI_ROOT=$Ui",
        ('-D' + $metadata.ui_option + '=' + $(if($NoUi){'OFF'}else{'ON'})),
        ('-DVBRECOMP_DEBUG_TOOLS=' + $(if($Production){'OFF'}else{'ON'})),
        ('-DVBRECOMP_CPUHOOK=' + $(if($Production){'OFF'}else{'ON'})))
    # Oracle builds are deliberately explicit: an old local static archive may
    # predate the debug snapshot ABI even though its filename is unchanged.
    if ($Oracle) { $options += '-DBEETLE_VB_ROOT=' + (Resolve-Path -LiteralPath $Oracle).Path }
    else { $options += '-DBEETLE_VB_ROOT=' + (Join-Path $buildPath 'oracle-not-requested') }
    if ($metadata.rom_option) { $options += '-D' + $metadata.rom_option + '=' + $Rom }
    if ($metadata.fast_build_option) { $options += '-D' + $metadata.fast_build_option + '=' + $(if($FastBuild){'ON'}else{'OFF'}) }
    Invoke-VbBuildTool (Join-Path $Toolchain 'cmake.exe') $options
    Invoke-VbBuildTool (Join-Path $Toolchain 'cmake.exe') @('--build',$buildPath,'--parallel',"$Jobs")
    foreach ($target in $metadata.extra_targets) {
        Invoke-VbBuildTool (Join-Path $Toolchain 'cmake.exe') @('--build',$buildPath,'--target',$target,'--parallel',"$Jobs")
    }
    $sdlSource = Join-Path $Toolchain 'SDL2.dll'
    $sdlDestination = Join-Path $buildPath 'vbrecomp/runtime/SDL2.dll'
    if (!(Test-Path -LiteralPath $sdlDestination) -or
        (Get-FileHash -LiteralPath $sdlSource -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $sdlDestination -Algorithm SHA256).Hash) {
        Copy-Item -LiteralPath $sdlSource -Destination $sdlDestination -Force
    }
    Invoke-VbBuildTool (Join-Path $Toolchain 'ctest.exe') @('--test-dir',$buildPath,'--output-on-failure')
} finally { $env:PATH = $savedPath }
