# Makes a self-signed code-signing certificate for TEST builds (.\Build-Installer.ps1 -SignCertificate ...), in dist\signing:
#   test-signing.pfx            the certificate with its private key (keep it private; never commit or share it)
#   test-signing.password.txt   the PFX's password
#   test-signing.cer            the certificate alone: what a test PC trusts (tools\Trust-TestCertificate.ps1)
# A self-signed signature is only trusted on a PC that trusts this certificate. Public builds need a certificate from a
# public authority (or SignPath); this one is for trying out how Windows and anti-cheat treat signed files.
param([int]$Years = 2)

$ErrorActionPreference = 'Stop'
$folder = Join-Path (Split-Path $PSScriptRoot -Parent) 'dist\signing'
$pfx = Join-Path $folder 'test-signing.pfx'
if (Test-Path $pfx) { throw "$pfx exists already. Delete dist\signing first to make a new one (and untrust the old one)." }
New-Item -ItemType Directory -Force $folder | Out-Null

$certificate = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=VR Gaze Mirror test signing (self-signed)' `
    -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -KeyExportPolicy Exportable `
    -NotAfter (Get-Date).AddYears($Years) -CertStoreLocation Cert:\CurrentUser\My
try {
    $bytes = New-Object byte[] 24
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    $password = [Convert]::ToBase64String($bytes)
    $secure = ConvertTo-SecureString $password -AsPlainText -Force
    Export-PfxCertificate -Cert $certificate -FilePath $pfx -Password $secure | Out-Null
    Export-Certificate -Cert $certificate -FilePath (Join-Path $folder 'test-signing.cer') | Out-Null
    [IO.File]::WriteAllText((Join-Path $folder 'test-signing.password.txt'), $password)
} finally {
    Remove-Item "Cert:\CurrentUser\My\$($certificate.Thumbprint)" # The PFX is the only copy of the key.
}
"Made in $folder - thumbprint $($certificate.Thumbprint), valid until $($certificate.NotAfter.ToString('yyyy-MM-dd'))"
