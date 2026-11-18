# Cheat Indicator Pro

A cheat indicator for Geometry Dash that checks gameplay more deeply than a basic indicator. It turns **red** as soon as it detects something abnormal while you play.

## Features

### Speed and physics scan
The mod compares the game speed against the player's physics. If the two do not match, or the values are invalid, the indicator turns red.

### Click scan
The mod monitors your click patterns. If it detects abnormal clicking (too consistent, too fast, or not humanly possible), it raises a warning.

### Installed mods scan
The mod checks the list of mods you have installed and makes a judgment based on it, helping to spot mods that may affect the fairness of a run.

## How to read the indicator

| Color | Meaning |
|-------|---------|
| Green | Nothing abnormal detected |
| Yellow | Something looks unusual, but there is not enough evidence (for example a suspicious mod is loaded) |
| Red | High-confidence detection of abnormal speed, physics, clicks or noclip |

## Notes

- This mod only gives **warnings and estimates**. It is not absolute proof. False positives can happen (for example due to lag or unstable FPS).
- Use the results as a reference when reviewing a run.

## Credits

- Code by [Claude](https://claude.ai)
- Management, idea by [tudeptraiii](user:29424521)
- Help from [Hoangdinhgg](user:30861869)

## Bug reports

If you find a bug or a false detection, please open an issue on the mod's GitHub page.
