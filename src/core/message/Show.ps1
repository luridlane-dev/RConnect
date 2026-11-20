$ErrorActionPreference = 'Stop'
$expectedUser = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%2'))
$account = [Security.Principal.WindowsIdentity]::GetCurrent().Name
if ([Diagnostics.Process]::GetCurrentProcess().SessionId -ne %1 -or
    ($account -ne $expectedUser -and ($account -split '\\')[-1] -ne $expectedUser)) {
    exit 1
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()

$form = New-Object Windows.Forms.Form
$form.Text = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%3'))
$form.StartPosition = 'CenterScreen'
$form.ClientSize = [Drawing.Size]::new(510, 270)
$form.MinimumSize = [Drawing.Size]::new(330, 200)
$form.Padding = [Windows.Forms.Padding]::new(12)
$form.TopMost = $true

$message = New-Object Windows.Forms.RichTextBox
$message.Dock = 'Fill'
$message.ReadOnly = $true
$message.DetectUrls = $true
$message.Text = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%4'))
$message.Select(0, 0)
$message.add_LinkClicked({
    param($sender, $event)
    $uri = $null
    if ([Uri]::TryCreate($event.LinkText, [UriKind]::Absolute, [ref]$uri) -and
        $uri.Scheme -in 'https', 'http') {
        Start-Process -FilePath $uri.AbsoluteUri
    }
})

$buttons = New-Object Windows.Forms.FlowLayoutPanel
$buttons.Dock = 'Bottom'
$buttons.FlowDirection = 'RightToLeft'
$buttons.AutoSize = $true
$buttons.Padding = [Windows.Forms.Padding]::new(0, 6, 0, 0)

$close = New-Object Windows.Forms.Button
$close.Text = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%5'))
$close.AutoSize = $true
$close.DialogResult = 'Cancel'
$form.CancelButton = $close
$buttons.Controls.Add($close)

$copy = New-Object Windows.Forms.Button
$copy.Text = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('%6'))
$copy.AutoSize = $true
$copy.add_Click({ [Windows.Forms.Clipboard]::SetText($message.Text) })
$buttons.Controls.Add($copy)

$form.Controls.Add($message)
$form.Controls.Add($buttons)
$form.add_Shown({ [Media.SystemSounds]::Exclamation.Play() })
[void]$form.ShowDialog()
$form.Dispose()
