# mnemon ZCode UserPromptSubmit hook for Windows PowerShell.

$null = [Console]::In.ReadToEnd()

[ordered]@{
    hookSpecificOutput = [ordered]@{
        hookEventName = "UserPromptSubmit"
        additionalContext = "[mnemon] Evaluate: recall needed? After responding, evaluate: remember needed?"
    }
} | ConvertTo-Json -Compress -Depth 4
