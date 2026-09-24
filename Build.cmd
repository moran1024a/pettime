@echo off
cd /d "%~dp0"
"%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /target:winexe /optimize+ /out:RoachPet_DoubleClick.exe /resource:assets\body-clean.png,RoachBody /resource:assets\pounce-atlas.png,RoachPounce /resource:assets\front-pounce-base.png,RoachFrontPounceBase /resource:assets\front-pounce-upstroke.png,RoachFrontPounceUp /resource:assets\front-pounce-downstroke.png,RoachFrontPounceDown /resource:assets\front-pounce-crouch.png,RoachFrontPounceCrouch /resource:assets\front-pounce-transition.png,RoachFrontPounceTransition /resource:assets\crush-atlas.png,RoachCrush /resource:assets\nymph-body.png,RoachNymph /reference:System.Drawing.dll /reference:System.Windows.Forms.dll RoachPet.cs
if errorlevel 1 pause
