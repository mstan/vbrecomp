param(
    [Parameter(Mandatory=$true)][string]$Destination,
    [string]$Toolchain='C:\msys64\mingw64\bin',
    [string]$Git='C:\Program Files\Git\cmd\git.exe'
)
$ErrorActionPreference='Stop'
function Invoke-Native([string]$Executable, [string[]]$Arguments) {
    # Windows PowerShell can turn ordinary native stderr (e.g. git progress)
    # into a terminating error when this script is called through a pipeline.
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Executable @Arguments
        $nativeExit = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previousPreference }
    if ($nativeExit -ne 0) { throw "$Executable failed ($nativeExit)" }
}
$Destination=[IO.Path]::GetFullPath($Destination)
if(Test-Path -LiteralPath $Destination) { throw 'Choose a new directory; existing oracle checkouts are preserved.' }
$revision='1275bd7bddf2166be5a10e45c26c5c2a61370658'
Invoke-Native $Git @('clone', 'https://github.com/libretro/beetle-vb-libretro.git', $Destination)
Invoke-Native $Git @('-C', $Destination, 'checkout', '--detach', $revision)
Invoke-Native $Git @('-C', $Destination, 'apply', (Join-Path $PSScriptRoot 'oracle-diagnostics.patch'))
$previousPath=$env:PATH
try {
    $env:PATH=$Toolchain+';'+$env:PATH
    # Make recipes interpret backslashes; forward-slash absolute paths reach
    # the intended native compiler without shell path rewriting.
    $makeToolchain = ([IO.Path]::GetFullPath($Toolchain)).Replace('\', '/')
    Push-Location -LiteralPath $Destination
    try {
        Invoke-Native (Join-Path $Toolchain 'mingw32-make.exe') @('-j', '8', 'platform=win', 'STATIC_LINKING=1',
            "CC=$makeToolchain/gcc.exe", "CXX=$makeToolchain/g++.exe", "AR=$makeToolchain/ar.exe")
    } finally { Pop-Location }
} finally { $env:PATH=$previousPath }
