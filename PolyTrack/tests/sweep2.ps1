param([string[]]$Cfg)
Set-Location $PSScriptRoot
foreach ($c in $Cfg) {
  $label, $envs = $c -split '\|', 2
  Get-ChildItem Env: | ? Name -match '^(FALL|NEEDK|FRAMES|EMERGE|GAP|FASTREL|FASTPRES|FASTCENTS|HDROP|ATTK|ATTF|BLEND|ATTMAX|HOP|RETAGE|RETX|ONSETDB|BLOCKMAX|VELCAL|VELRES|REPICK|REPAGE|REPOWN|REPLO|REPFLOOR|REPPEAK|REPREL|REPWAIT|HIGHP|MASKF|MASKM|HOLD|MISSF|STRICT|SHARED|MISSC|MISSBLK|SHRATIO|TRACE|TRACEMS)$' | % { Remove-Item "Env:$($_.Name)" }
  if ($envs) { foreach ($kv in ($envs.Trim() -split ';')) { if ($kv) { $k, $val = $kv -split '=', 2; Set-Item "Env:$($k.Trim())" $val.Trim() } } }
  $env:BRIEF = '1'
  "== $label"
  .\trk.exe -20 0.5 0.5 0 1 | Select-String 'SINGLE|CHORDS:|OFFS|HELD|RETRIG' | % { '   ' + $_.Line }
  .\seq.exe 1 0 | Select-String 'TOTAL' | % { '   SEQ ' + $_.Line }
}
