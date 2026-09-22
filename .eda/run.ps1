param(
    [Parameter(Mandatory = $true)][string]$JsFile,
    [int]$BridgePort = 49620,
    [int]$TimeoutSec = 90
)

$code = [System.IO.File]::ReadAllText($JsFile, [System.Text.Encoding]::UTF8)
$payload = @{ code = $code } | ConvertTo-Json -Compress
$bytes = [System.Text.Encoding]::UTF8.GetBytes($payload)
if ($env:EDA_DEBUG) { Write-Output "CODE_LEN: $($code.Length) PAYLOAD_LEN: $($payload.Length)" }
try {
    $resp = Invoke-RestMethod -Uri "http://127.0.0.1:$BridgePort/execute" -Method Post -ContentType "application/json; charset=utf-8" -Body $bytes -TimeoutSec $TimeoutSec
    $resp | ConvertTo-Json -Depth 30
}
catch {
    $r = $_.Exception.Response
    if ($r) {
        $sr = New-Object System.IO.StreamReader($r.GetResponseStream())
        Write-Output "HTTP_STATUS: $([int]$r.StatusCode)"
        Write-Output "BODY: $($sr.ReadToEnd())"
    }
    else {
        Write-Output "ERROR: $($_.Exception.Message)"
    }
    Write-Output "PAYLOAD_LEN: $($payload.Length)"
}
