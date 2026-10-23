# Makes THIS PC trust the self-signed test certificate (dist\signing\test-signing.cer), or stops trusting it (-Remove).
# Run it yourself in an elevated PowerShell:
#   powershell -ExecutionPolicy Bypass -File tools\Trust-TestCertificate.ps1
#   powershell -ExecutionPolicy Bypass -File tools\Trust-TestCertificate.ps1 -Remove
# Trusting it puts it among the PC's root authorities: anything signed with its key counts as trusted here. The key is in
# dist\signing\test-signing.pfx only - keep that file to yourself, and run -Remove when the testing is done.
param([switch]$Remove)

$ErrorActionPreference = 'Stop'
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this from an elevated (administrator) PowerShell.' }
$cer = Join-Path (Split-Path $PSScriptRoot -Parent) 'dist\signing\test-signing.cer'
$certificate = New-Object Security.Cryptography.X509Certificates.X509Certificate2($cer)

# Root: the chain ends in a trusted authority. TrustedPublisher: Windows takes it as a known publisher (installer prompts).
foreach ($store in 'Root', 'TrustedPublisher') {
    $path = "Cert:\LocalMachine\$store\$($certificate.Thumbprint)"
    if ($Remove) {
        if (Test-Path $path) { Remove-Item $path; "Removed from LocalMachine\$store" } else { "Not in LocalMachine\$store" }
    } else {
        Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
        "Trusted in LocalMachine\$store"
    }
}
