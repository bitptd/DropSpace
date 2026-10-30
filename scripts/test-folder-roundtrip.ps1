param(
    [Parameter(Mandatory = $true)][string]$ServerExe,
    [Parameter(Mandatory = $true)][string]$RuntimeDir,
    [Parameter(Mandatory = $true)][string]$ZipFile,
    [Parameter(Mandatory = $true)][Int64]$ObjectSizeBytes,
    [Parameter(Mandatory = $true)][string]$ObjectSha256,
    [Parameter(Mandatory = $true)][Int64]$ExpandedSizeBytes,
    [Parameter(Mandatory = $true)][Int64]$EntryCount,
    [Parameter(Mandatory = $true)][string]$TreeSha256
)

$ErrorActionPreference = 'Stop'
$serverPath = (Resolve-Path -LiteralPath $ServerExe).Path
$runtimePath = (Resolve-Path -LiteralPath $RuntimeDir).Path
$zipPath = (Resolve-Path -LiteralPath $ZipFile).Path

Push-Location $runtimePath
try {
    $pairOutput = & $serverPath pair
} finally {
    Pop-Location
}
$pairingToken = $pairOutput[-1].Trim()
$server = Start-Process -FilePath $serverPath -ArgumentList 'serve' -WorkingDirectory $runtimePath -WindowStyle Hidden -PassThru
try {
    Start-Sleep -Seconds 1
    $pairBody = @{
        pairing_token = $pairingToken
        display_name = 'Windows folder integration'
        platform = 'windows'
    } | ConvertTo-Json -Compress
    $paired = Invoke-RestMethod -Method Post -Uri 'http://127.0.0.1:8787/v1/pair' -ContentType 'application/json' -Body $pairBody
    $headers = @{ Authorization = "Bearer $($paired.device_token)" }
    $metadata = @{
        client_upload_id = [guid]::NewGuid().ToString()
        kind = 'folder'
        name = 'source-folder'
        object_format = 'zip'
        object_size_bytes = $ObjectSizeBytes
        object_sha256 = $ObjectSha256
        expanded_size_bytes = $ExpandedSizeBytes
        entry_count = $EntryCount
        tree_sha256 = $TreeSha256
    }
    $created = Invoke-RestMethod -Method Post -Uri 'http://127.0.0.1:8787/v1/files' -Headers $headers -ContentType 'application/json' -Body ($metadata | ConvertTo-Json -Compress)
    $ready = Invoke-RestMethod -Method Put -Uri "http://127.0.0.1:8787/v1/files/$($created.id)/content" -Headers $headers -ContentType 'application/octet-stream' -InFile $zipPath
    $snapshot = Invoke-RestMethod -Method Get -Uri 'http://127.0.0.1:8787/v1/snapshot' -Headers $headers
    if ($ready.state -ne 'ready' -or $ready.tree_sha256 -ne $TreeSha256 -or $snapshot.files.Count -lt 1) {
        throw 'Folder round-trip did not publish the expected ready metadata.'
    }
    [pscustomobject]@{
        FileId = $ready.id
        State = $ready.state
        Kind = $ready.kind
        SnapshotCount = $snapshot.files.Count
        TreeSha256 = $ready.tree_sha256
    }
} finally {
    Stop-Process -Id $server.Id -ErrorAction SilentlyContinue
    Wait-Process -Id $server.Id -ErrorAction SilentlyContinue
}

