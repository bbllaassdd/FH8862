param(
    [int]$Port = 8862,
    [switch]$NoBrowser
)

$ErrorActionPreference = "Stop"
$DashboardRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $DashboardRoot
$RecordingsRoot = Join-Path $ProjectRoot "recordings"
$ConfigPath = Join-Path $DashboardRoot "config.json"
$HeaderPath = Join-Path $ProjectRoot "src\project_stream_config.h"
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$RecordJobs = @{}
New-Item -ItemType Directory -Force -Path $RecordingsRoot | Out-Null

function Find-Vlc {
    $programFilesX86 = [Environment]::GetEnvironmentVariable("ProgramFiles(x86)")
    $candidates = @(
        "D:\Program Files\VideoLAN\VLC\vlc.exe",
        (Join-Path $env:ProgramFiles "VideoLAN\VLC\vlc.exe"),
        (Join-Path $programFilesX86 "VideoLAN\VLC\vlc.exe")
    )
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return $null
}

function Read-HttpRequest($NetworkStream) {
    # 页面请求均为 UTF-8 文本；导入的视频使用 Base64 放在 JSON 中传输。
    $reader = New-Object IO.StreamReader($NetworkStream, $Utf8NoBom, $false, 8192, $true)
    $requestLine = $reader.ReadLine()
    if (-not $requestLine) { return $null }
    $parts = $requestLine.Split(" ")
    if ($parts.Count -lt 2) { throw "HTTP 请求行格式错误" }
    $headers = @{}
    while ($true) {
        $line = $reader.ReadLine()
        if ($null -eq $line -or $line -eq "") { break }
        $separator = $line.IndexOf(":")
        if ($separator -gt 0) {
            $headers[$line.Substring(0, $separator).Trim().ToLowerInvariant()] =
                $line.Substring($separator + 1).Trim()
        }
    }
    $contentLength = 0
    if ($headers.ContainsKey("content-length")) {
        $contentLength = [int]$headers["content-length"]
    }
    $body = ""
    if ($contentLength -gt 0) {
        $buffer = New-Object char[] $contentLength
        $offset = 0
        while ($offset -lt $contentLength) {
            $count = $reader.Read($buffer, $offset, $contentLength - $offset)
            if ($count -le 0) { break }
            $offset += $count
        }
        $body = New-Object string($buffer, 0, $offset)
    }
    $requestPath = $parts[1].Split("?")[0].ToLowerInvariant()
    return @{ Method=$parts[0]; Path=$requestPath;
        Headers=$headers; Body=$body }
}

function Send-Bytes($NetworkStream, [byte[]]$Bytes, [string]$ContentType, [int]$StatusCode = 200) {
    $reason = if ($StatusCode -eq 200) { "OK" } elseif ($StatusCode -eq 404) { "Not Found" } else { "Error" }
    $header = "HTTP/1.1 $StatusCode $reason`r`n" +
        "Content-Type: $ContentType`r`n" +
        "Content-Length: $($Bytes.Length)`r`n" +
        "Cache-Control: no-store`r`n" +
        "Connection: close`r`n`r`n"
    $headerBytes = [Text.Encoding]::ASCII.GetBytes($header)
    $NetworkStream.Write($headerBytes, 0, $headerBytes.Length)
    $NetworkStream.Write($Bytes, 0, $Bytes.Length)
    $NetworkStream.Flush()
}

function Send-Json($NetworkStream, $Value, [int]$StatusCode = 200) {
    $json = $Value | ConvertTo-Json -Depth 8
    Send-Bytes $NetworkStream $Utf8NoBom.GetBytes($json) "application/json; charset=utf-8" $StatusCode
}

function Send-Error($NetworkStream, [string]$Message, [int]$StatusCode = 400) {
    Send-Json $NetworkStream @{ ok = $false; error = $Message } $StatusCode
}

function Read-Config {
    return Get-Content -LiteralPath $ConfigPath -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Require-Int($Value, [string]$Name, [int]$Min, [int]$Max) {
    $parsed = 0
    if (-not [int]::TryParse([string]$Value, [ref]$parsed) -or
        $parsed -lt $Min -or $parsed -gt $Max) {
        throw "$Name 必须是 $Min 到 $Max 之间的整数"
    }
    return $parsed
}

function Save-Config($InputConfig) {
    $ip = [string]$InputConfig.receiverIp
    $parsedIp = $null
    if (-not [System.Net.IPAddress]::TryParse($ip, [ref]$parsedIp)) {
        throw "接收端 IP 地址格式不正确"
    }
    $config = [ordered]@{
        receiverIp = $ip
        mainPort = Require-Int $InputConfig.mainPort "主码流端口" 1024 65534
        mainWidth = Require-Int $InputConfig.mainWidth "主码流宽度" 320 3840
        mainHeight = Require-Int $InputConfig.mainHeight "主码流高度" 180 2160
        mainFps = Require-Int $InputConfig.mainFps "主码流帧率" 1 60
        mainBitrate = Require-Int $InputConfig.mainBitrate "主码流码率" 64000 20000000
        subWidth = Require-Int $InputConfig.subWidth "子码流宽度" 160 1920
        subHeight = Require-Int $InputConfig.subHeight "子码流高度" 90 1080
        subFps = Require-Int $InputConfig.subFps "子码流帧率" 1 60
        subBitrate = Require-Int $InputConfig.subBitrate "子码流码率" 32000 8000000
    }
    if (($config.mainWidth % 2) -ne 0 -or ($config.mainHeight % 2) -ne 0 -or
        ($config.subWidth % 2) -ne 0 -or ($config.subHeight % 2) -ne 0) {
        throw "H.264 分辨率的宽和高必须是偶数"
    }
    [IO.File]::WriteAllText($ConfigPath, (($config | ConvertTo-Json) + [Environment]::NewLine), $Utf8NoBom)
    $header = @"
#ifndef PROJECT_STREAM_CONFIG_H
#define PROJECT_STREAM_CONFIG_H

/*
 * 本文件由 Windows 答辩控制台生成，编码为 UTF-8。
 * 修改后必须复制 Project 到 Ubuntu 并重新编译，再把新程序放到开发板。
 */
#define HOMEWORK_MAIN_WIDTH   $($config.mainWidth)
#define HOMEWORK_MAIN_HEIGHT  $($config.mainHeight)
#define HOMEWORK_MAIN_FPS     $($config.mainFps)
#define HOMEWORK_MAIN_BITRATE $($config.mainBitrate)

#define HOMEWORK_SUB_WIDTH    $($config.subWidth)
#define HOMEWORK_SUB_HEIGHT   $($config.subHeight)
#define HOMEWORK_SUB_FPS      $($config.subFps)
#define HOMEWORK_SUB_BITRATE  $($config.subBitrate)

#endif /* PROJECT_STREAM_CONFIG_H */
"@
    [IO.File]::WriteAllText($HeaderPath, $header, $Utf8NoBom)
    return $config
}

function Get-StreamInfo([string]$Stream) {
    $config = Read-Config
    if ($Stream -eq "main") {
        return @{ name="主码流"; port=[int]$config.mainPort; width=[int]$config.mainWidth;
            height=[int]$config.mainHeight; fps=[int]$config.mainFps; bitrate=[int]$config.mainBitrate }
    }
    if ($Stream -eq "sub") {
        return @{ name="子码流"; port=([int]$config.mainPort + 1); width=[int]$config.subWidth;
            height=[int]$config.subHeight; fps=[int]$config.subFps; bitrate=[int]$config.subBitrate }
    }
    throw "未知码流：$Stream"
}

function Get-SafeRecordingPath([string]$Name) {
    $safeName = [IO.Path]::GetFileName($Name)
    if (-not $safeName -or $safeName -ne $Name) { throw "录像文件名不安全" }
    $extension = [IO.Path]::GetExtension($safeName).ToLowerInvariant()
    if ($extension -notin @(".ts", ".h264", ".mp4", ".mkv")) {
        throw "只支持 TS、H264、MP4、MKV 视频文件"
    }
    return Join-Path $RecordingsRoot $safeName
}

function Get-Recordings {
    $extensions = @(".ts", ".h264", ".mp4", ".mkv")
    return @(Get-ChildItem -LiteralPath $RecordingsRoot -File -ErrorAction SilentlyContinue |
        Where-Object { $extensions -contains $_.Extension.ToLowerInvariant() } |
        Sort-Object LastWriteTime -Descending | ForEach-Object {
            @{ name=$_.Name; size=$_.Length; modified=$_.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss");
               extension=$_.Extension.TrimStart(".").ToUpperInvariant() }
        })
}

function Get-JobStatus {
    $result = @{}
    foreach ($stream in @("main", "sub")) {
        if (-not $RecordJobs.ContainsKey($stream)) {
            $result[$stream] = @{ state="idle"; elapsed=0; progress=0 }
            continue
        }
        $job = $RecordJobs[$stream]
        $elapsed = [Math]::Floor(((Get-Date).ToUniversalTime() - $job.Started).TotalSeconds)
        $state = if ($job.Process.HasExited) { "finished" } else { "recording" }
        $result[$stream] = @{ state=$state; elapsed=[Math]::Min(60,$elapsed);
            progress=[Math]::Min(100,[Math]::Round($elapsed/60*100));
            file=$job.File; port=$job.Port }
    }
    return $result
}

function Start-VlcPreview([string]$Stream) {
    $vlc = Find-Vlc
    if (-not $vlc) { throw "未找到 VLC，请检查安装路径" }
    $info = Get-StreamInfo $Stream
    Start-Process -FilePath $vlc -ArgumentList @("--network-caching=250", "udp://@:$($info.port)") | Out-Null
}

function Start-VlcRecording([string]$Stream) {
    $vlc = Find-Vlc
    if (-not $vlc) { throw "未找到 VLC，请检查安装路径" }
    if ($RecordJobs.ContainsKey($Stream) -and -not $RecordJobs[$Stream].Process.HasExited) {
        throw "$Stream 已经在录像"
    }
    $info = Get-StreamInfo $Stream
    $fileName = "project_pc_$($Stream)_$(Get-Date -Format 'yyyyMMdd_HHmmss')_60s.ts"
    $outputPath = Join-Path $RecordingsRoot $fileName
    $arguments = @("--intf","dummy","--dummy-quiet","--network-caching=300","--run-time=60",
        "--sout=#standard{access=file,mux=ts,dst=$outputPath}",
        "udp://@:$($info.port)","vlc://quit")
    $process = Start-Process -FilePath $vlc -ArgumentList $arguments -PassThru
    $RecordJobs[$Stream] = @{ Process=$process; Started=(Get-Date).ToUniversalTime();
        File=$fileName; Port=$info.port }
    return $fileName
}

function Serve-Static($NetworkStream, [string]$RelativePath) {
    if (-not $RelativePath -or $RelativePath -eq "/") { $RelativePath = "index.html" }
    $RelativePath = $RelativePath.TrimStart("/") -replace "/", "\"
    $candidate = [IO.Path]::GetFullPath((Join-Path $DashboardRoot $RelativePath))
    $root = [IO.Path]::GetFullPath($DashboardRoot) + [IO.Path]::DirectorySeparatorChar
    if (-not $candidate.StartsWith($root,[StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        Send-Error $NetworkStream "页面不存在" 404
        return
    }
    $types = @{".html"="text/html; charset=utf-8";".css"="text/css; charset=utf-8";
        ".js"="application/javascript; charset=utf-8";".png"="image/png"}
    $extension = [IO.Path]::GetExtension($candidate).ToLowerInvariant()
    $type = if ($types.ContainsKey($extension)) { $types[$extension] } else { "application/octet-stream" }
    Send-Bytes $NetworkStream ([IO.File]::ReadAllBytes($candidate)) $type
}

$listener = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, $Port)
$prefix = "http://localhost:$Port/"
try {
    $listener.Start()
    Write-Host ""
    Write-Host "FH8862 presentation console is running" -ForegroundColor Cyan
    Write-Host "URL: $prefix"
    Write-Host "Recordings: $RecordingsRoot"
    Write-Host "Press Ctrl+C to stop."
    if (-not $NoBrowser) { Start-Process $prefix | Out-Null }

    while ($true) {
        $client = $listener.AcceptTcpClient()
        $networkStream = $client.GetStream()
        try {
            $request = Read-HttpRequest $networkStream
            if ($null -eq $request) { continue }
            $path = $request.Path
            if ($request.Method -eq "GET" -and $path -eq "/api/status") {
                $config = Read-Config
                Send-Json $networkStream @{ok=$true;vlcFound=[bool](Find-Vlc);vlcPath=Find-Vlc;
                    recordingsDir=$RecordingsRoot;config=$config;jobs=Get-JobStatus;files=@(Get-Recordings);
                    boardCommand="./project_fh8862 $($config.receiverIp) $($config.mainPort)"}
                continue
            }
            if ($request.Method -eq "POST" -and $path -eq "/api/config") {
                $saved = Save-Config ($request.Body | ConvertFrom-Json)
                Send-Json $networkStream @{ok=$true;config=$saved;
                    message="参数已保存并生成 src/project_stream_config.h"}
                continue
            }
            if ($request.Method -eq "POST" -and $path -eq "/api/vlc/open") {
                $body = $request.Body | ConvertFrom-Json
                Start-VlcPreview ([string]$body.stream)
                Send-Json $networkStream @{ok=$true}
                continue
            }
            if ($request.Method -eq "POST" -and $path -eq "/api/record/start") {
                $body = $request.Body | ConvertFrom-Json
                Send-Json $networkStream @{ok=$true;file=(Start-VlcRecording ([string]$body.stream))}
                continue
            }
            if ($request.Method -eq "POST" -and $path -eq "/api/files/open") {
                $body = $request.Body | ConvertFrom-Json
                $filePath = Get-SafeRecordingPath ([string]$body.name)
                if (-not (Test-Path -LiteralPath $filePath)) { throw "录像文件不存在" }
                $vlc = Find-Vlc
                if (-not $vlc) { throw "未找到 VLC" }
                Start-Process -FilePath $vlc -ArgumentList @($filePath) | Out-Null
                Send-Json $networkStream @{ok=$true}
                continue
            }
            if ($request.Method -eq "POST" -and $path -eq "/api/folder/open") {
                Start-Process -FilePath "explorer.exe" -ArgumentList @($RecordingsRoot) | Out-Null
                Send-Json $networkStream @{ok=$true}
                continue
            }
            if ($request.Method -eq "POST" -and $path -eq "/api/import") {
                $body = $request.Body | ConvertFrom-Json
                $fileName = [Uri]::UnescapeDataString([string]$body.name)
                $destination = Get-SafeRecordingPath $fileName
                [IO.File]::WriteAllBytes($destination, [Convert]::FromBase64String([string]$body.data))
                Send-Json $networkStream @{ok=$true;file=[IO.Path]::GetFileName($destination)}
                continue
            }
            if ($path.StartsWith("/api/")) {
                Send-Error $networkStream "接口不存在" 404
                continue
            }
            Serve-Static $networkStream $path
        }
        catch {
            try { Send-Error $networkStream $_.Exception.Message 500 } catch {}
        }
        finally {
            $networkStream.Dispose()
            $client.Close()
        }
    }
}
finally {
    $listener.Stop()
}
