param(
  [Parameter(Mandatory = $true)]
  [string]$PairingToken,
  [string]$Server = 'https://43.108.80.211:8443'
)

$ErrorActionPreference = 'Stop'

function Get-Sha256Hex([byte[]]$Bytes) {
  $sha = [System.Security.Cryptography.SHA256]::Create()
  try { return ([Convert]::ToHexString($sha.ComputeHash($Bytes))).ToLowerInvariant() }
  finally { $sha.Dispose() }
}

function Receive-WebSocketText(
  [System.Net.WebSockets.ClientWebSocket]$Socket,
  [int]$TimeoutSeconds = 10
) {
  $buffer = [byte[]]::new(8192)
  $stream = [System.IO.MemoryStream]::new()
  $cancel = [System.Threading.CancellationTokenSource]::new(
      [TimeSpan]::FromSeconds($TimeoutSeconds))
  try {
    do {
      $segment = [ArraySegment[byte]]::new($buffer)
      $result = $Socket.ReceiveAsync($segment, $cancel.Token).GetAwaiter().GetResult()
      if ($result.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) {
        throw 'WebSocket closed before the expected message'
      }
      $stream.Write($buffer, 0, $result.Count)
    } while (-not $result.EndOfMessage)
    return [Text.Encoding]::UTF8.GetString($stream.ToArray())
  } finally {
    $cancel.Dispose()
    $stream.Dispose()
  }
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('dropspace-public-' + [Guid]::NewGuid())
[IO.Directory]::CreateDirectory($tempRoot) | Out-Null
$headers = $null
$deviceId = $null
$fileId = $null
$clipboardId = $null
$pngId = $null
$socket = $null

try {
  $pairBody = @{
    pairing_token = $PairingToken
    display_name = 'Windows phase-two public acceptance'
    platform = 'windows'
  } | ConvertTo-Json -Compress
  $paired = Invoke-RestMethod -Uri "$Server/v1/pair" -Method Post `
      -ContentType 'application/json' -Body $pairBody
  $deviceId = $paired.device_id
  $headers = @{ Authorization = "Bearer $($paired.device_token)" }
  $me = Invoke-RestMethod -Uri "$Server/v1/me" -Headers $headers
  if ($me.device_id -ne $deviceId) { throw 'paired identity mismatch' }
  Write-Output 'PASS pairing and bearer authentication'

  $fileBytes = [Text.Encoding]::UTF8.GetBytes('DropSpace phase-two file API regression')
  $filePath = Join-Path $tempRoot 'file-api.bin'
  [IO.File]::WriteAllBytes($filePath, $fileBytes)
  $fileHash = Get-Sha256Hex $fileBytes
  $intent = @{
    client_upload_id = 'phase2-regression-' + [Guid]::NewGuid().ToString('N')
    kind = 'file'
    name = 'phase-two-regression.txt'
    object_format = 'raw'
    object_size_bytes = $fileBytes.Length
    object_sha256 = $fileHash
    expanded_size_bytes = $fileBytes.Length
    entry_count = 1
    tree_sha256 = $null
  } | ConvertTo-Json -Compress
  $createdFile = Invoke-RestMethod -Uri "$Server/v1/files" -Method Post `
      -Headers $headers -ContentType 'application/json' -Body $intent
  $fileId = $createdFile.id
  Invoke-RestMethod -Uri "$Server/v1/files/$fileId/content" -Method Put `
      -Headers $headers -ContentType 'application/octet-stream' -InFile $filePath | Out-Null
  $downloadPath = Join-Path $tempRoot 'file-download.bin'
  Invoke-WebRequest -Uri "$Server/v1/files/$fileId/content" -Headers $headers `
      -OutFile $downloadPath | Out-Null
  $downloadedFile = [IO.File]::ReadAllBytes($downloadPath)
  if ((Get-Sha256Hex $downloadedFile) -ne $fileHash) { throw 'file API SHA-256 regression' }
  $snapshot = Invoke-RestMethod -Uri "$Server/v1/snapshot" -Headers $headers
  if ($snapshot.files.id -notcontains $fileId) { throw 'uploaded file absent from snapshot' }
  Write-Output 'PASS existing file upload, snapshot, and download regression'

  $socket = [System.Net.WebSockets.ClientWebSocket]::new()
  $socket.Options.SetRequestHeader('Authorization', $headers.Authorization)
  $webSocketBase = $Server -replace '^https:', 'wss:' -replace '^http:', 'ws:'
  $webSocketUri = [Uri]($webSocketBase + '/v1/clipboard/ws')
  $connectCancel = [System.Threading.CancellationTokenSource]::new([TimeSpan]::FromSeconds(10))
  try { [void]$socket.ConnectAsync($webSocketUri, $connectCancel.Token).GetAwaiter().GetResult() }
  finally { $connectCancel.Dispose() }
  $initial = Receive-WebSocketText $socket | ConvertFrom-Json
  if ($initial.type -ne 'clipboard.snapshot' -or $initial.clipboard_protocol_version -ne 1) {
    throw 'invalid initial WebSocket snapshot'
  }

  $clipboardBytes = [Text.Encoding]::UTF8.GetBytes("DropSpace public clipboard 中文 `nline 2")
  $clipboardPath = Join-Path $tempRoot 'clipboard.bin'
  [IO.File]::WriteAllBytes($clipboardPath, $clipboardBytes)
  $clipboardId = [Guid]::NewGuid().ToString()
  $clipboardHash = Get-Sha256Hex $clipboardBytes
  $metadata = @{
    clipboard_protocol_version = 1
    clipboard_id = $clipboardId
    content_type = 'text'
    copied_at = [DateTimeOffset]::UtcNow.ToString('o')
    content_hash = $clipboardHash
    content_size = $clipboardBytes.Length
  } | ConvertTo-Json -Compress
  $clipboardResponse = Invoke-RestMethod -Uri "$Server/v1/clipboard" -Method Post `
      -Headers $headers -Form @{ metadata = $metadata; content = Get-Item $clipboardPath }
  if (-not $clipboardResponse.retained -or $clipboardResponse.item.clipboard_id -ne $clipboardId) {
    throw 'clipboard upload response mismatch'
  }

  $createdEvent = $null
  for ($attempt = 0; $attempt -lt 4 -and $null -eq $createdEvent; $attempt++) {
    $event = Receive-WebSocketText $socket | ConvertFrom-Json
    if ($event.type -eq 'clipboard.created') { $createdEvent = $event }
  }
  if ($null -eq $createdEvent -or $createdEvent.item.clipboard_id -ne $clipboardId) {
    throw 'WebSocket create event not received'
  }
  $history = Invoke-RestMethod -Uri "$Server/v1/clipboard/history?limit=20" -Headers $headers
  if ($history.items[0].clipboard_id -ne $clipboardId) { throw 'clipboard history order mismatch' }
  $latest = Invoke-RestMethod -Uri "$Server/v1/clipboard/latest" -Headers $headers
  if ($latest.item.clipboard_id -ne $clipboardId) { throw 'clipboard latest mismatch' }
  $clipboardDownload = Join-Path $tempRoot 'clipboard-download.bin'
  Invoke-WebRequest -Uri "$Server$($latest.item.content_reference)" -Headers $headers `
      -OutFile $clipboardDownload | Out-Null
  if ((Get-Sha256Hex ([IO.File]::ReadAllBytes($clipboardDownload))) -ne $clipboardHash) {
    throw 'clipboard content SHA-256 mismatch'
  }
  Write-Output 'PASS public HTTPS clipboard upload, history, latest, and verified download'
  Write-Output 'PASS public authenticated WSS snapshot and create notification'

  $pngBytes = [Convert]::FromBase64String(
      'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=')
  $pngPath = Join-Path $tempRoot 'clipboard.png'
  [IO.File]::WriteAllBytes($pngPath, $pngBytes)
  $pngId = [Guid]::NewGuid().ToString()
  $pngHash = Get-Sha256Hex $pngBytes
  $pngMetadata = @{
    clipboard_protocol_version = 1
    clipboard_id = $pngId
    content_type = 'image/png'
    copied_at = [DateTimeOffset]::UtcNow.AddMilliseconds(1).ToString('o')
    content_hash = $pngHash
    content_size = $pngBytes.Length
  } | ConvertTo-Json -Compress
  $pngResponse = Invoke-RestMethod -Uri "$Server/v1/clipboard" -Method Post `
      -Headers $headers -Form @{ metadata = $pngMetadata; content = Get-Item $pngPath }
  if ($pngResponse.item.clipboard_id -ne $pngId -or -not $pngResponse.item.thumbnail) {
    throw 'PNG clipboard upload or thumbnail reference mismatch'
  }
  $pngDownload = Join-Path $tempRoot 'clipboard-image-download.png'
  $thumbnailDownload = Join-Path $tempRoot 'clipboard-thumbnail.png'
  Invoke-WebRequest -Uri "$Server$($pngResponse.item.content_reference)" -Headers $headers `
      -OutFile $pngDownload | Out-Null
  Invoke-WebRequest -Uri "$Server$($pngResponse.item.thumbnail)" -Headers $headers `
      -OutFile $thumbnailDownload | Out-Null
  if ((Get-Sha256Hex ([IO.File]::ReadAllBytes($pngDownload))) -ne $pngHash -or
      (Get-Item -LiteralPath $thumbnailDownload).Length -le 0) {
    throw 'PNG content or thumbnail download mismatch'
  }
  Write-Output 'PASS public PNG clipboard object and generated thumbnail'

  Invoke-RestMethod -Uri "$Server/v1/clipboard/$clipboardId" -Method Delete -Headers $headers | Out-Null
  $clipboardId = $null
  Invoke-RestMethod -Uri "$Server/v1/clipboard/$pngId" -Method Delete -Headers $headers | Out-Null
  $pngId = $null
  Invoke-RestMethod -Uri "$Server/v1/files/$fileId" -Method Delete -Headers $headers | Out-Null
  $fileId = $null
  Write-Output 'PASS clipboard item deletion and file cleanup requests'
} finally {
  if ($socket) { $socket.Dispose() }
  if ($headers -and $fileId) {
    try { Invoke-RestMethod -Uri "$Server/v1/files/$fileId" -Method Delete -Headers $headers | Out-Null } catch {}
  }
  if ($headers -and $clipboardId) {
    try { Invoke-RestMethod -Uri "$Server/v1/clipboard/$clipboardId" -Method Delete -Headers $headers | Out-Null } catch {}
  }
  if ($headers -and $pngId) {
    try { Invoke-RestMethod -Uri "$Server/v1/clipboard/$pngId" -Method Delete -Headers $headers | Out-Null } catch {}
  }
  if ($headers -and $deviceId) {
    try { Invoke-WebRequest -Uri "$Server/v1/devices/$deviceId" -Method Delete -Headers $headers | Out-Null } catch {}
  }
  if (Test-Path -LiteralPath $tempRoot) {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force
  }
}
