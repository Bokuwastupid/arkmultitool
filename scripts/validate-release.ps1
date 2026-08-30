param(
    [string]$Payload = (Join-Path $PSScriptRoot '..\build-msvc\dist\kopt_payload_candidate.dll'),
    [string]$GameExe = 'A:\steam\steamapps\common\ARK\ShooterGame\Binaries\Win64\ShooterGame.exe'
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$expectedGameHash = '9BC401417A776C5244A1B0B3255DC3AF4A9D73E3F5C1BA96228FBE3FB1A43477'

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Read-RvaBytes([string]$Path, [uint32]$Rva, [int]$Count) {
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        $stream.Position = 0x3C
        $pe = $reader.ReadInt32()
        $stream.Position = $pe + 6
        $sectionCount = $reader.ReadUInt16()
        $stream.Position = $pe + 20
        $optionalSize = $reader.ReadUInt16()
        $sectionTable = $pe + 24 + $optionalSize
        for ($index = 0; $index -lt $sectionCount; $index++) {
            $stream.Position = $sectionTable + (40 * $index)
            $null = $reader.ReadBytes(8)
            $virtualSize = $reader.ReadUInt32()
            $virtualAddress = $reader.ReadUInt32()
            $rawSize = $reader.ReadUInt32()
            $rawPointer = $reader.ReadUInt32()
            $sectionSize = [Math]::Max($virtualSize, $rawSize)
            if ($Rva -ge $virtualAddress -and $Rva -lt ($virtualAddress + $sectionSize)) {
                $stream.Position = $rawPointer + ($Rva - $virtualAddress)
                return $reader.ReadBytes($Count)
            }
        }
        throw ('RVA 0x{0:X} is outside PE sections' -f $Rva)
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

function Assert-RvaPrefix([uint32]$Rva, [byte[]]$Expected, [string]$Name) {
    $actual = Read-RvaBytes $GameExe $Rva $Expected.Length
    Assert-True ($null -ne $actual -and $actual.Length -eq $Expected.Length) "$Name bytes unavailable"
    for ($index = 0; $index -lt $Expected.Length; $index++) {
        if ($actual[$index] -ne $Expected[$index]) {
            throw ('{0} mismatch at RVA 0x{1:X}: expected {2}, got {3}' -f
                $Name, $Rva, (($Expected | ForEach-Object { $_.ToString('X2') }) -join ' '),
                (($actual | ForEach-Object { $_.ToString('X2') }) -join ' '))
        }
    }
}

$payloadPath = [IO.Path]::GetFullPath($Payload)
Assert-True (Test-Path -LiteralPath $payloadPath) "Payload does not exist: $payloadPath"
Assert-True (Test-Path -LiteralPath $GameExe) "ShooterGame.exe does not exist: $GameExe"
Assert-True ((Get-FileHash -Algorithm SHA256 -LiteralPath $GameExe).Hash -eq $expectedGameHash) 'ShooterGame.exe SHA-256 mismatch'

$injector = Join-Path $projectRoot 'build-msvc\dist\kopt_injector.exe'
Assert-True (Test-Path -LiteralPath $injector) 'Injector self-test executable is missing'
& $injector --self-test --dll $payloadPath
Assert-True ($LASTEXITCODE -eq 0) 'PE64 payload self-test failed'

Assert-RvaPrefix 0x1087E60 ([byte[]](0x48,0x89,0x54,0x24,0x10,0x48)) 'AShooterPlayerController::SetControlRotation'
Assert-RvaPrefix 0x29077D0 ([byte[]](0x40,0x55,0x56,0x57,0x41,0x56)) 'AController::LineOfSightTo'
Assert-RvaPrefix 0x286B1F0 ([byte[]](0x48,0x89,0x5C,0x24,0x08,0x57)) 'AActor::GetActorBounds'
Assert-RvaPrefix 0xD29420 ([byte[]](0x48,0x89,0x4C,0x24,0x08,0x48)) 'AShooterCharacter::GetRecoilMultiplier'
Assert-RvaPrefix 0x11C7450 ([byte[]](0x48,0x89,0x4C,0x24,0x08,0x48)) 'AShooterWeapon::GetFireCameraShakeScale'
Assert-RvaPrefix 0x2ADB2C0 ([byte[]](0x40,0x53,0x48,0x83,0xEC,0x20)) 'UPrimitiveComponent::SetRenderCustomDepth'
Assert-RvaPrefix 0x2ADB5A0 ([byte[]](0x40,0x53,0x48,0x83,0xEC,0x20)) 'UPrimitiveComponent::SetCustomDepthStencilValue'

$payloadBytes = [IO.File]::ReadAllBytes($payloadPath)
$ascii = [Text.Encoding]::ASCII.GetString($payloadBytes)
$unicode = [Text.Encoding]::Unicode.GetString($payloadBytes)
$runtimeSource = Get-Content -Raw (Join-Path $projectRoot 'src\runtime.cpp')
Assert-True ($runtimeSource.Contains('actor.address), snapshot_.camera.location, false)')) `
    'LineOfSightTo must receive the camera as its ViewPoint argument'
foreach ($forbidden in @('Engine.ini', 'Live CFG', 'r.ScreenPercentage', 'foliage.DensityScale')) {
    Assert-True (-not $ascii.Contains($forbidden) -and -not $unicode.Contains($forbidden)) "Forbidden game-config marker is linked: $forbidden"
}
Assert-True ($unicode.Contains('SchemaVersion') -and $unicode.Contains('.legacy.bak')) 'Versioned settings migration is not linked into the payload'
foreach ($required in @(
    'Mounted FOV',
    'Projectile prediction',
    'PLAYER ESP PREVIEW',
    'Battle Mode',
    'Live search: name / tribe / class',
    'Threat panel',
    'Camera smoothing',
    'No recoil',
    'No weapon sway',
    'Turret target lock',
    'Dino aim activation',
    'DinoActivationMode',
    'DinoKey',
    'Aim activation entered fresh active state'
)) {
    Assert-True ($unicode.Contains($required)) "Required runtime feature marker is missing: $required"
}

$overlaySource = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'src\overlay.cpp')
$payloadSource = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'src\payload.cpp')
$runtimeSource = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'src\runtime.cpp')
$injectorSource = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'src\injector.cpp')
Assert-True ($overlaySource.Contains('active_slider_ == 0') -and $overlaySource.Contains('consume_click(hit, input)')) 'Slider fresh-click capture guard is missing'
Assert-True ($overlaySource.Contains('menu_resizing_') -and $overlaySource.Contains('combo_popup_') -and
    $overlaySource.Contains('consume_right_click(row, input)')) 'Resizable menu, overlay combo or RMB bind context is missing'
Assert-True ($overlaySource.Contains('feature_catalog()') -and
    $overlaySource.Contains('update_feature_hotkeys(Settings& settings)') -and
    $overlaySource.Contains('draw_hotkey_list(const Settings& settings')) `
    'Universal feature binds or active hotkey list is missing'
Assert-True ($overlaySource.Contains('Use selected structure list') -and
    $overlaySource.Contains('exact_token_contains(settings.selected_structure_types') -and
    $overlaySource.Contains('structure_catalog_')) 'Searchable exact structure catalog filter is missing'
Assert-True ($overlaySource.Contains('profile_delete_confirmation_') -and
    $overlaySource.Contains('std::filesystem::remove(selected_profile')) `
    'Protected local-profile deletion workflow is missing'
Assert-True ($payloadSource.Contains('g_overlay.update_feature_hotkeys(g_settings)')) `
    'Universal feature binds are not updated from the payload loop'
Assert-True ($payloadSource.Contains('g_polled_left_down.store(false') -and $payloadSource.Contains('sided_modifier_key')) 'Pointer/modifier input reset guard is missing'
Assert-True ($payloadSource.Contains('g_menu_pointer_armed') -and
    $payloadSource.Contains('g_menu_pointer_armed.store(!left_down')) 'Menu carried-click release gate is missing'
Assert-True ($payloadSource.Contains('if (key == VK_LBUTTON)') -and
    $payloadSource.Contains('g_polled_left_down.store(true') -and
    $payloadSource.Contains('assigning Mouse 1 cannot also activate')) 'Mouse 1 rebind click quarantine is missing'
Assert-True ($payloadSource.Contains('Freecam consumes the same physical controls as the pawn') -and
    $payloadSource.Contains('release_message') -and $payloadSource.Contains('allow_system_close')) 'Freecam pawn-input isolation is missing'
Assert-True ($runtimeSource.Contains('aim_enable_changed') -and $runtimeSource.Contains('menu_just_closed')) 'Aim fresh-activation guard is missing'
Assert-True ($runtimeSource.Contains('const bool dead = actor_is_dead(actor)') -and
    $runtimeSource.Contains('if (!dead && distance_m <= settings.alert_radius_m)') -and
    $overlaySource.Contains('actor.kind == ActorKind::player && !actor_is_dead(actor)')) `
    'Unified dead-player exclusion for radius, threat and radar counts is missing'
Assert-True ($runtimeSource.Contains('dino_activated') -and $runtimeSource.Contains('dino_aim_toggle_active_')) `
    'Independent dino aim activation state is missing'
Assert-True ($runtimeSource.Contains('snapshot_.world_address != active_world_') -and $runtimeSource.Contains('abandon_chams')) 'Reconnect-safe chams world guard is missing'
Assert-True ($injectorSource.Contains($expectedGameHash) -and $injectorSource.Contains('loaded_payload') -and
    $injectorSource.Contains('KoptRequestUnload')) 'Injector build pin, duplicate-payload guard or unload contract is missing'
Assert-True ($injectorSource.Contains('kopt_payload_candidate.dll') -and
    $injectorSource.Contains('std::filesystem::exists(candidate)')) 'Direct-launch candidate payload fallback is missing'

$cmake = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'CMakeLists.txt')
$buildScript = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'build.ps1')
Assert-True (-not $cmake.Contains('graphics_config.cpp')) 'graphics_config.cpp is still in CMake payload sources'
Assert-True (-not $buildScript.Contains('graphics_config.obj')) 'graphics_config.obj is still linked by build.ps1'
Assert-True (-not (Test-Path -LiteralPath (Join-Path $projectRoot 'src\graphics_config.cpp'))) 'Retired game INI implementation still exists'
Assert-True (-not (Test-Path -LiteralPath (Join-Path $projectRoot 'include\kopt\graphics_config.hpp'))) 'Retired game INI header still exists'

$armorHeader = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'include\kopt\armor_icon_assets.generated.h')
$weaponHeader = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'include\kopt\weapon_icon_assets.generated.h')
Assert-True (($armorHeader | Select-String -AllMatches 'inline constexpr std::array<std::uint8_t, 9216> ').Matches.Count -eq 17) 'Armor atlas does not contain all 17 assets'
Assert-True (($weaponHeader | Select-String -AllMatches 'inline constexpr std::array<std::uint8_t, 16384> ').Matches.Count -eq 10) 'Weapon atlas does not contain all 10 assets'

$runtimeSmokePath = Join-Path $projectRoot 'scripts\runtime-smoke.ps1'
Assert-True (Test-Path -LiteralPath $runtimeSmokePath) 'Runtime smoke script is missing'
$runtimeSmoke = Get-Content -Raw -LiteralPath $runtimeSmokePath
$null = [scriptblock]::Create($runtimeSmoke)
foreach ($requiredSmokeMarker in @(
    'initialLogLength',
    'kopt_payload_candidate.dll',
    'Payload worker started',
    'D3D11 overlay initialized',
    'Camera game-tick hook installed',
    'World generation changed:',
    'Local player runtime became valid',
    '--unload',
    'Manual interaction gate'
)) {
    Assert-True ($runtimeSmoke.Contains($requiredSmokeMarker)) "Runtime smoke marker is missing: $requiredSmokeMarker"
}

Write-Host ('[KOPT] Release validation passed: {0}' -f $payloadPath)
