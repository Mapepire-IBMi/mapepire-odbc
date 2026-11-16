<#
.SYNOPSIS
    Registers the Mapepire ODBC driver on Windows, or creates a per-user DSN for it.

.DESCRIPTION
    -InstallDriver  registers "Mapepire ODBC Driver" for the whole machine
                    (HKLM\SOFTWARE\ODBC\ODBCINST.INI). Requires an elevated shell.
                    Afterwards DRIVER={Mapepire ODBC Driver} works in connection strings.

    -AddUserDsn     creates a user DSN (HKCU) that points straight at the DLL.
                    No administrator rights are needed, and the driver does not have
                    to be registered first.

    -RemoveUserDsn / -UninstallDriver undo the above.

    The bitness of the DLL must match the applications that use it (64-bit DLL
    for 64-bit applications).

.EXAMPLE
    .\register-windows.ps1 -InstallDriver -DllPath C:\mapepire\mapepireodbc.dll

.EXAMPLE
    .\register-windows.ps1 -AddUserDsn -Name MyIBMi -DllPath .\build\mapepireodbc.dll `
        -Server myibmi.example.com -Port 8076 -CertSHA256 '2137DA8C...'
#>
[CmdletBinding(DefaultParameterSetName = 'Dsn')]
param(
    [Parameter(ParameterSetName = 'Install', Mandatory)] [switch] $InstallDriver,
    [Parameter(ParameterSetName = 'Uninstall', Mandatory)] [switch] $UninstallDriver,
    [Parameter(ParameterSetName = 'Dsn', Mandatory)] [switch] $AddUserDsn,
    [Parameter(ParameterSetName = 'RemoveDsn', Mandatory)] [switch] $RemoveUserDsn,

    [Parameter(ParameterSetName = 'Install', Mandatory)]
    [Parameter(ParameterSetName = 'Dsn', Mandatory)]
    [string] $DllPath,

    [Parameter(ParameterSetName = 'Dsn', Mandatory)]
    [Parameter(ParameterSetName = 'RemoveDsn', Mandatory)]
    [string] $Name,

    [Parameter(ParameterSetName = 'Dsn')] [string] $Server,
    [Parameter(ParameterSetName = 'Dsn')] [int] $Port = 8076,
    [Parameter(ParameterSetName = 'Dsn')] [string] $CertSHA256,
    [Parameter(ParameterSetName = 'Dsn')] [switch] $TrustServerCertificate,
    [Parameter(ParameterSetName = 'Dsn')] [string] $Props,

    [string] $DriverName = 'Mapepire ODBC Driver'
)

$ErrorActionPreference = 'Stop'

switch ($PSCmdlet.ParameterSetName) {
    'Install' {
        $dll = (Resolve-Path $DllPath).Path
        $key = "HKLM:\SOFTWARE\ODBC\ODBCINST.INI\$DriverName"
        New-Item -Path $key -Force | Out-Null
        Set-ItemProperty -Path $key -Name Driver -Value $dll
        Set-ItemProperty -Path $key -Name APILevel -Value '1'
        Set-ItemProperty -Path $key -Name ConnectFunctions -Value 'YYN'
        Set-ItemProperty -Path $key -Name DriverODBCVer -Value '03.80'
        Set-ItemProperty -Path $key -Name FileUsage -Value '0'
        Set-ItemProperty -Path $key -Name SQLLevel -Value '1'
        New-Item -Path 'HKLM:\SOFTWARE\ODBC\ODBCINST.INI\ODBC Drivers' -Force -ErrorAction SilentlyContinue | Out-Null
        Set-ItemProperty -Path 'HKLM:\SOFTWARE\ODBC\ODBCINST.INI\ODBC Drivers' -Name $DriverName -Value 'Installed'
        Write-Host "Registered '$DriverName' -> $dll"
    }
    'Uninstall' {
        Remove-Item -Path "HKLM:\SOFTWARE\ODBC\ODBCINST.INI\$DriverName" -Recurse -ErrorAction SilentlyContinue
        Remove-ItemProperty -Path 'HKLM:\SOFTWARE\ODBC\ODBCINST.INI\ODBC Drivers' -Name $DriverName -ErrorAction SilentlyContinue
        Write-Host "Unregistered '$DriverName'"
    }
    'Dsn' {
        $dll = (Resolve-Path $DllPath).Path
        $key = "HKCU:\Software\ODBC\ODBC.INI\$Name"
        New-Item -Path $key -Force | Out-Null
        Set-ItemProperty -Path $key -Name Driver -Value $dll
        if ($Server) { Set-ItemProperty -Path $key -Name Server -Value $Server }
        Set-ItemProperty -Path $key -Name Port -Value "$Port"
        if ($CertSHA256) { Set-ItemProperty -Path $key -Name CertSHA256 -Value $CertSHA256 }
        if ($TrustServerCertificate) { Set-ItemProperty -Path $key -Name TrustServerCertificate -Value 'yes' }
        if ($Props) { Set-ItemProperty -Path $key -Name Props -Value $Props }
        New-Item -Path 'HKCU:\Software\ODBC\ODBC.INI\ODBC Data Sources' -Force -ErrorAction SilentlyContinue | Out-Null
        Set-ItemProperty -Path 'HKCU:\Software\ODBC\ODBC.INI\ODBC Data Sources' -Name $Name -Value $DriverName
        Write-Host "Created user DSN '$Name' (credentials are not stored; pass UID/PWD when connecting)"
    }
    'RemoveDsn' {
        Remove-Item -Path "HKCU:\Software\ODBC\ODBC.INI\$Name" -Recurse -ErrorAction SilentlyContinue
        Remove-ItemProperty -Path 'HKCU:\Software\ODBC\ODBC.INI\ODBC Data Sources' -Name $Name -ErrorAction SilentlyContinue
        Write-Host "Removed user DSN '$Name'"
    }
}
