# Saves a screenshot of the primary screen to the given path (dev aid for checking the games visually).
param([string]$Path = "$env:TEMP\shot.png")
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
[System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
$bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
