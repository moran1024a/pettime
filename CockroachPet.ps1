$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot 'RoachPet_DoubleClick.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    & (Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe') /nologo /target:winexe /optimize+ ("/out:" + $exe) ("/resource:" + (Join-Path $PSScriptRoot 'assets\body-clean.png') + ',RoachBody') ("/resource:" + (Join-Path $PSScriptRoot 'assets\pounce-atlas.png') + ',RoachPounce') ("/resource:" + (Join-Path $PSScriptRoot 'assets\front-pounce-base.png') + ',RoachFrontPounceBase') ("/resource:" + (Join-Path $PSScriptRoot 'assets\front-pounce-upstroke.png') + ',RoachFrontPounceUp') ("/resource:" + (Join-Path $PSScriptRoot 'assets\front-pounce-downstroke.png') + ',RoachFrontPounceDown') ("/resource:" + (Join-Path $PSScriptRoot 'assets\front-pounce-crouch.png') + ',RoachFrontPounceCrouch') ("/resource:" + (Join-Path $PSScriptRoot 'assets\front-pounce-transition.png') + ',RoachFrontPounceTransition') ("/resource:" + (Join-Path $PSScriptRoot 'assets\crush-atlas.png') + ',RoachCrush') ("/resource:" + (Join-Path $PSScriptRoot 'assets\nymph-body.png') + ',RoachNymph') /reference:System.Drawing.dll /reference:System.Windows.Forms.dll (Join-Path $PSScriptRoot 'RoachPet.cs')
    if ($LASTEXITCODE -ne 0) { throw 'Unable to build RoachPet_DoubleClick.exe.' }
}
& $exe
