namespace GazeOverlay;

public enum GazeLiveState { NoGame, Tracking, EyesNotTracked, Stale }

/// <summary>The Status tab's live line: what the 2.0 mirror is doing (see MirrorLive). Read-only, read on demand.</summary>
public static class GazeLive
{
    public static (GazeLiveState State, string Text) Read()
    {
        var live = MirrorLive.Read();
        if (live is not { Producing: true })
        {
            return (GazeLiveState.NoGame, "No VR game is running. The mirror starts with the game (OpenXR) or with the SteamVR helper (SteamVR games).");
        }
        var kind = live.OpenXR ? "OpenXR game" : "SteamVR game";
        var size = live.Width > 0 ? $", picture {live.Width} x {live.Height}" : ", no reader yet (open the OBS source or the mirror window)";
        return live.GazeValid
            ? (GazeLiveState.Tracking, $"Mirroring {live.Program} ({kind}){size}, gaze arriving.")
            : live.GazeSupport == 2
            ? (GazeLiveState.EyesNotTracked, $"Mirroring {live.Program} ({kind}){size}, no gaze: the OpenXR runtime says this headset gives none. A headset without eye tracking can use Gaze from = Fixed centre on the Mirror page.")
            : (GazeLiveState.EyesNotTracked, $"Mirroring {live.Program} ({kind}){size}, but no gaze right now - eye tracking off in the headset software, or the headset is not on. A headset without eye tracking can use Gaze from = Fixed centre on the Mirror page.");
    }
}
