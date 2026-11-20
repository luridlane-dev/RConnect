$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
trap {
    $reason = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($_.Exception.Message))
    Write-Output "RCONNECT_MESSAGE_ERROR $reason"
    exit 1
}

$sessionId = %1
$expectedUser = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%2'))
$popup = @'
%3
'@
$encodedPopup = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($popup))

$desktop = Get-CimInstance Win32_Process -Filter "SessionId=$sessionId AND Name='explorer.exe'" |
    Select-Object -First 1
if (-not $desktop) {
    throw 'The selected session no longer has a Windows desktop.'
}
$owner = Invoke-CimMethod -InputObject $desktop -MethodName GetOwner
$account = "$($owner.Domain)\$($owner.User)"
if ($owner.ReturnValue -ne 0 -or -not $owner.User -or
    ($expectedUser -ne $owner.User -and $expectedUser -ne $account)) {
    throw 'The account in the selected session has changed.'
}

$scheduler = New-Object -ComObject 'Schedule.Service'
$scheduler.Connect()
$folder = $scheduler.GetFolder('\')
$definition = $scheduler.NewTask(0)
$definition.Principal.UserId = $account
$definition.Principal.LogonType = 3 # TASK_LOGON_INTERACTIVE_TOKEN
$definition.Settings.DisallowStartIfOnBatteries = $false
$definition.Settings.StopIfGoingOnBatteries = $false
$definition.Settings.ExecutionTimeLimit = 'PT5M'
$definition.Settings.DeleteExpiredTaskAfter = 'PT1M'

# The past start time leaves RunEx as the only way to start this task.
# The end boundary lets Windows remove it even if the sender disconnects.
$definition.Settings.StartWhenAvailable = $false
$trigger = $definition.Triggers.Create(1) # TASK_TRIGGER_TIME
$now = [DateTimeOffset]::Now
$trigger.StartBoundary = $now.AddMinutes(-2).ToString('yyyy-MM-ddTHH:mm:sszzz')
$trigger.EndBoundary = $now.AddMinutes(6).ToString('yyyy-MM-ddTHH:mm:sszzz')

$action = $definition.Actions.Create(0) # TASK_ACTION_EXEC
$action.Path = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$action.Arguments = '-NoProfile -NonInteractive -STA -WindowStyle Hidden -EncodedCommand $(Arg0)'
$name = '%4'
$task = $folder.RegisterTaskDefinition($name, $definition, 2, $account, $null, 3, $null)
try {
    $instance = $task.RunEx($encodedPopup, 4, $sessionId, $account) # TASK_RUN_USE_SESSION_ID
    if (-not $instance) {
        throw 'Windows did not start the interactive message task.'
    }
} catch {
    $failure = $_
    try { $folder.DeleteTask($name, 0) } catch { }
    throw $failure
}
Write-Output 'RCONNECT_MESSAGE_QUEUED'
