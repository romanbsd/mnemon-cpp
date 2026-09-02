# mnemon ZCode Stop hook for Windows PowerShell.

$raw = [Console]::In.ReadToEnd()
try {
    $payload = $raw | ConvertFrom-Json
} catch {
    $payload = $null
}

if ($null -ne $payload -and $payload.stop_hook_active -eq $true) {
    exit 0
}

$lastMessage = ""
if ($null -ne $payload -and $null -ne $payload.last_assistant_message) {
    $lastMessage = ([string]$payload.last_assistant_message).ToLowerInvariant()
}
if ($lastMessage.Contains("mnemon") -or $lastMessage.Contains("durable memory")) {
    exit 0
}

[ordered]@{
    decision = "block"
    reason = "[mnemon] Briefly evaluate whether this exchange warrants durable memory. If yes, use the mnemon skill/CLI to remember only durable, non-secret facts; otherwise say no durable memory is needed."
} | ConvertTo-Json -Compress
