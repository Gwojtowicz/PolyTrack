# Runs trk.exe (pickup model, -20 dBFS) once per tuning line and prints the summary.
# Each line: label | env assignments (FALL=..;NEEDK=..;FRAMES=..;EMERGE=..)
param([string[]]$Cfg)
Set-Location $PSScriptRoot
foreach ($c in $Cfg) {
  $label, $envs = $c -split '\|', 2
  foreach ($v in 'FALL', 'NEEDK', 'FRAMES', 'EMERGE', 'GAP', 'FASTREL', 'FASTPRES', 'FASTCENTS', 'HDROP', 'ATTK', 'ATTF', 'BLEND', 'ATTMAX', 'HOP', 'RETAGE', 'RETX', 'ONSETDB', 'BLOCKMAX') { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }
  if ($envs) { foreach ($kv in ($envs.Trim() -split ';')) { if ($kv) { $k, $val = $kv -split '=', 2; Set-Item "Env:$($k.Trim())" $val.Trim() } } }
  $env:BRIEF = '1'
  $o = .\trk.exe -20 0.5 0.5 0 1
  "== $label"
  $o | Select-String 'SINGLE|CHORDS|MEDIAN' | ForEach-Object { '   ' + $_.Line }
}

