<# install_development_drivers.ps1
   Creates/updates ODBC driver registry keys for Trino based on the current directory.
   Also writes a .reg file for manual review/import.
#>

param(
  [string]$DriverName = "",          # Leave empty to auto-pick based on DLL path
  [string]$DllPath = "",             # Optional explicit path to TrinoODBC.dll
  [string]$DriverODBCVer = "03.80",
  [string]$ConnectFunctions = "YYN",
  [string]$APILevel = "0",
  [string]$SQLLevel = "0",
  [string]$FileUsage = "0",
  [string]$RegOutPath = ""           # Optional path for the .reg file; defaults next to this script
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-Admin {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  $principal = New-Object Security.Principal.WindowsPrincipal($id)
  if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "This script must be run as Administrator."
  }
}

function Classify-Build([string]$dllFullPath) {
  if ($dllFullPath -match '(?i)(\\|/)Debug(\\|/)' )   { return 'Debug' }
  if ($dllFullPath -match '(?i)(\\|/)Release(\\|/)' ) { return 'Release' }
  return 'Unknown'
}

function Resolve-Dll([string]$PathFromUser) {
  if ($PathFromUser) {
    if (-not (Test-Path -LiteralPath $PathFromUser)) {
      throw ("Specified DllPath not found: {0}" -f $PathFromUser)
    }
    $p = (Resolve-Path -LiteralPath $PathFromUser).Path
    return [pscustomobject]@{ Path = $p; Build = (Classify-Build $p) }
  }

  $candidates = @(
    ".\build\Debug\TrinoODBC.dll",
    ".\build\Release\TrinoODBC.dll",
    ".\TrinoODBC.dll"
  )

  foreach ($c in $candidates) {
    if (Test-Path -LiteralPath $c) {
      $p = (Resolve-Path -LiteralPath $c).Path
      return [pscustomobject]@{ Path = $p; Build = (Classify-Build $p) }
    }
  }

  $list = ($candidates -join "`n  ")
  throw ("Could not find TrinoODBC.dll. Pass -DllPath or place the DLL in one of:`n  {0}" -f $list)
}

function Choose-DriverName([string]$UserProvided, [string]$BuildKind) {
  if ($UserProvided) { return $UserProvided }
  switch ($BuildKind) {
    'Debug'   { return 'TrinoODBCDebug' }
    'Release' { return 'TrinoODBCRelease' }
    default   { return 'TrinoODBCRelease' } # default when unknown
  }
}

function Set-DriverKeys([string]$BaseKey, [string]$DriverName, [hashtable]$Props) {
  $driversKey = Join-Path $BaseKey "ODBC Drivers"
  New-Item -Path $driversKey -Force | Out-Null
  New-ItemProperty -Path $driversKey -Name $DriverName -Value "Installed" -PropertyType String -Force | Out-Null

  $driverKey = Join-Path $BaseKey $DriverName
  New-Item -Path $driverKey -Force | Out-Null

  foreach ($k in $Props.Keys) {
    New-ItemProperty -Path $driverKey -Name $k -Value $Props[$k] -PropertyType String -Force | Out-Null
  }

  Write-Host ("Updated: {0} -> {1}=Installed" -f $driversKey, $DriverName)
  Write-Host ("Updated: {0}" -f $driverKey)
}

function Write-RegFile(
  [string]$OutPath,
  [string]$DriverName,
  [string]$DllFullPath,
  [string]$DriverODBCVer,
  [string]$ConnectFunctions,
  [string]$APILevel,
  [string]$SQLLevel,
  [string]$FileUsage,
  [bool]$IncludeWow6432
) {
  # 1) Collapse any runs of backslashes to a single backslash
  $normalized = ($DllFullPath -replace '\\+', '\')
  # 2) Escape ONCE for .reg: single '\' -> double '\\'
  $dllReg = ($normalized -replace '\\', '\\')

  $lines = @()
  $lines += 'Windows Registry Editor Version 5.00'
  $lines += ''

  # 64-bit hive
  $lines += '[HKEY_LOCAL_MACHINE\SOFTWARE\ODBC\ODBCINST.INI\ODBC Drivers]'
  $lines += ('"{0}"="Installed"' -f $DriverName)
  $lines += ''
  $lines += ('[HKEY_LOCAL_MACHINE\SOFTWARE\ODBC\ODBCINST.INI\{0}]' -f $DriverName)
  $lines += ('"APILevel"="{0}"' -f $APILevel)
  $lines += ('"Driver"="{0}"' -f $dllReg)
  $lines += ('"Setup"="{0}"' -f $dllReg)
  $lines += ('"DriverODBCVer"="{0}"' -f $DriverODBCVer)
  $lines += ('"ConnectFunctions"="{0}"' -f $ConnectFunctions)
  $lines += ('"FileUsage"="{0}"' -f $FileUsage)
  $lines += ('"SQLLevel"="{0}"' -f $SQLLevel)

  if ($IncludeWow6432) {
    $lines += ''
    $lines += '[HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\ODBC\ODBCINST.INI\ODBC Drivers]'
    $lines += ('"{0}"="Installed"' -f $DriverName)
    $lines += ''
    $lines += ('[HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\ODBC\ODBCINST.INI\{0}]' -f $DriverName)
    $lines += ('"APILevel"="{0}"' -f $APILevel)
    $lines += ('"Driver"="{0}"' -f $dllReg)
    $lines += ('"Setup"="{0}"' -f $dllReg)
    $lines += ('"DriverODBCVer"="{0}"' -f $DriverODBCVer)
    $lines += ('"ConnectFunctions"="{0}"' -f $ConnectFunctions)
    $lines += ('"FileUsage"="{0}"' -f $FileUsage)
    $lines += ('"SQLLevel"="{0}"' -f $SQLLevel)
  }

  $content = ($lines -join "`r`n")
  Set-Content -Path $OutPath -Value $content -Encoding Unicode
  Write-Host ("Wrote .reg file: {0}" -f $OutPath)
}

try {
  Assert-Admin

  $dllInfo = Resolve-Dll $DllPath
  $effectiveDriverName = Choose-DriverName -UserProvided $DriverName -BuildKind $dllInfo.Build

  $props = @{
    "APILevel"         = $APILevel
    "Driver"           = $dllInfo.Path
    "Setup"            = $dllInfo.Path
    "DriverODBCVer"    = $DriverODBCVer
    "ConnectFunctions" = $ConnectFunctions
    "FileUsage"        = $FileUsage
    "SQLLevel"         = $SQLLevel
  }

  # 64-bit ODBC driver registration
  $base64 = "HKLM:\SOFTWARE\ODBC\ODBCINST.INI"
  Set-DriverKeys -BaseKey $base64 -DriverName $effectiveDriverName -Props $props

  # 32-bit ODBC driver registration on 64-bit OS (WOW6432Node)
  $includeWow = $false
  if ([Environment]::Is64BitOperatingSystem) {
    $includeWow = $true
    $base32 = "HKLM:\SOFTWARE\WOW6432Node\ODBC\ODBCINST.INI"
    Set-DriverKeys -BaseKey $base32 -DriverName $effectiveDriverName -Props $props
  } else {
    Write-Host "Info: Detected 32-bit OS. WOW6432Node not used."
  }

  # Pick a default .reg output path if none supplied
  if (-not $RegOutPath) {
    $namePart = $effectiveDriverName -replace '[^A-Za-z0-9._-]', '_'
    $RegOutPath = Join-Path -Path $PSScriptRoot -ChildPath ("TrinoODBC-{0}.reg" -f $namePart)
  }

  Write-RegFile -OutPath $RegOutPath `
                -DriverName $effectiveDriverName `
                -DllFullPath $dllInfo.Path `
                -DriverODBCVer $DriverODBCVer `
                -ConnectFunctions $ConnectFunctions `
                -APILevel $APILevel `
                -SQLLevel $SQLLevel `
                -FileUsage $FileUsage `
                -IncludeWow6432:$includeWow

  Write-Host ""
  Write-Host "Done."
  Write-Host ("  DLL:       {0}" -f $dllInfo.Path)
  Write-Host ("  Build:     {0}" -f $dllInfo.Build)
  Write-Host ("  DriverKey: {0}" -f $effectiveDriverName)
  Write-Host ("  .reg:      {0}" -f $RegOutPath)
}
catch {
  Write-Error ($_ | Out-String)
  exit 1
}