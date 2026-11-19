//! juanrun - headless SSH OS test runner built on the Snow emulator core.
//!
//! Boots a ROM with an optional floppy and SCSI disks, runs for a number of
//! CPU cycles (or until a marker appears on the modem serial port), then
//! writes the final screen as PREFIX.png and everything the machine sent on
//! the modem port as PREFIX.serial.txt.
//!
//! This file lives in the SSH OS project (tools/snowrun/main.rs). `make tools`
//! copies it into the Snow source tree as an extra binary and builds it.

use std::fs::{self, File};
use std::path::PathBuf;

use anyhow::{Context, Result};
use clap::Parser;

use snow_core::emulator::{Emulator, MouseMode};
use snow_core::emulator::comm::{EmulatorCommand, EmulatorEvent, EmulatorSpeed};
use snow_core::keymap::{KeyEvent, Keymap};
use snow_core::mac::MacModel;
use snow_core::mac::scc::SccCh;
use snow_core::tickable::{Tickable, Ticks};

#[derive(Parser)]
struct Args {
    /// System ROM file
    #[arg(long)]
    rom: String,

    /// Floppy image for the first drive
    #[arg(long)]
    floppy: Option<String>,

    /// SCSI hard disk image as ID:PATH (can be repeated)
    #[arg(long)]
    hdd: Vec<String>,

    /// Attach a network adapter (as on a BlueSCSI with WiFi) at this SCSI ID.
    /// It is linked to Snow's built-in gateway, 10.0.0.1
    #[arg(long)]
    net: Option<usize>,

    /// Save the adapter's traffic to this file, readable by Wireshark
    #[arg(long)]
    pcap: Option<String>,

    /// Maximum CPU cycles to run (the SE runs about 7.8 million per second)
    #[arg(long, default_value_t = 400_000_000)]
    cycles: Ticks,

    /// Wait for this text on the modem port. Without --type, stop there
    #[arg(long)]
    until: Option<String>,

    /// Once --until has appeared, type this on the keyboard. Escapes:
    /// \n Return, \m move the mouse 40 right and 25 down, \M(dx,dy) move
    /// it by that much, \c click, \^ \v \< \> arrow keys, \q Control-],
    /// \oX the key X with Option held,
    /// \p pause for about a second, \s save a screenshot (PREFIX-1.png,
    /// PREFIX-2.png, ...). After each Return the typing pauses
    /// until a prompt appears, so a slow command does not have the next one
    /// typed over it
    #[arg(long)]
    r#type: Option<String>,

    /// What counts as a prompt after Return (may be repeated; default "> ")
    #[arg(long)]
    prompt: Vec<String>,

    /// After typing, stop once the modem-port output contains this text
    #[arg(long)]
    done: Option<String>,

    /// Output prefix: writes PREFIX.png and PREFIX.serial.txt
    #[arg(long)]
    out: String,
}

/// Cycles to keep running after the stop marker so the screen catches up.
const SETTLE_CYCLES: Ticks = 600_000;
/// Cycles between key transitions while typing (about 40 ms).
const KEY_CYCLES: Ticks = 320_000;

const KEY_SHIFT: u8 = 0x38;
const KEY_RETURN: u8 = 0x24;

/// ADB key code for a character on a US keyboard, and whether it needs Shift.
fn key_for(c: char) -> Option<(u8, bool)> {
    const PLAIN: &[(char, u8)] = &[
        ('a', 0x00), ('s', 0x01), ('d', 0x02), ('f', 0x03), ('h', 0x04), ('g', 0x05),
        ('z', 0x06), ('x', 0x07), ('c', 0x08), ('v', 0x09), ('b', 0x0B), ('q', 0x0C),
        ('w', 0x0D), ('e', 0x0E), ('r', 0x0F), ('y', 0x10), ('t', 0x11), ('1', 0x12),
        ('2', 0x13), ('3', 0x14), ('4', 0x15), ('6', 0x16), ('5', 0x17), ('=', 0x18),
        ('9', 0x19), ('7', 0x1A), ('-', 0x1B), ('8', 0x1C), ('0', 0x1D), (']', 0x1E),
        ('o', 0x1F), ('u', 0x20), ('[', 0x21), ('i', 0x22), ('p', 0x23), ('\n', 0x24),
        ('l', 0x25), ('j', 0x26), ('\'', 0x27), ('k', 0x28), (';', 0x29), ('\\', 0x2A),
        (',', 0x2B), ('/', 0x2C), ('n', 0x2D), ('m', 0x2E), ('.', 0x2F), ('\t', 0x30),
        (' ', 0x31), ('`', 0x32), ('\u{8}', 0x33), ('\u{1b}', 0x35),
    ];
    const SHIFTED: &str = "1!2@3#4$5%6^7&8*9(0)-_=+[{]}\\|;:'\",<.>/?`~";

    let find = |c: char| PLAIN.iter().find(|(p, _)| *p == c).map(|(_, k)| *k);
    if let Some(k) = find(c) {
        return Some((k, false));
    }
    if c.is_ascii_uppercase() {
        return find(c.to_ascii_lowercase()).map(|k| (k, true));
    }
    let chars: Vec<char> = SHIFTED.chars().collect();
    chars
        .chunks(2)
        .find(|pair| pair[1] == c)
        .and_then(|pair| find(pair[0]))
        .map(|k| (k, true))
}

/// One step of scripted input.
enum Step {
    Key(u8, bool),
    Mouse(i16, i16),
    Button(bool),
    Pause,
    Shot,
}

const KEY_CONTROL: u8 = 0x36;
const KEY_OPTION: u8 = 0x3A;

/// Turn text into a list of input steps.
fn keystrokes(text: &str) -> Result<Vec<Step>> {
    let mut out = Vec::new();
    let chars: Vec<char> = text.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        let mut c = chars[i];
        let mut option = false;
        i += 1;
        // \o and a character: that key with Option held.
        if c == '\\' && i + 1 < chars.len() && chars[i] == 'o' {
            option = true;
            c = chars[i + 1];
            i += 2;
        }
        if c == '\\' && i < chars.len() {
            let e = chars[i];
            i += 1;
            let arrow = match e {
                '^' => Some(0x3E),
                'v' => Some(0x3D),
                '<' => Some(0x3B),
                '>' => Some(0x3C),
                _ => None,
            };
            if let Some(key) = arrow {
                out.push(Step::Key(key, true));
                out.push(Step::Key(key, false));
                continue;
            }
            match e {
                'n' => c = '\n',
                'm' => {
                    out.push(Step::Mouse(40, 25));
                    continue;
                }
                'M' => {
                    let rest: String = chars[i..].iter().collect();
                    let close = rest.find(')').context("\\M wants (dx,dy)")?;
                    let (dx, dy) = rest[1..close].split_once(',').context("\\M wants (dx,dy)")?;
                    out.push(Step::Mouse(dx.trim().parse()?, dy.trim().parse()?));
                    i += close + 1;
                    continue;
                }
                'c' => {
                    out.push(Step::Button(true));
                    out.push(Step::Button(false));
                    continue;
                }
                'p' => {
                    out.push(Step::Pause);
                    continue;
                }
                's' => {
                    out.push(Step::Shot);
                    continue;
                }
                'q' => {
                    out.push(Step::Key(KEY_CONTROL, true));
                    out.push(Step::Key(0x1E, true));
                    out.push(Step::Key(0x1E, false));
                    out.push(Step::Key(KEY_CONTROL, false));
                    continue;
                }
                other => c = other,
            }
        }
        let (key, shift) = key_for(c).with_context(|| format!("cannot type {c:?}"))?;
        if shift {
            out.push(Step::Key(KEY_SHIFT, true));
        }
        if option {
            out.push(Step::Key(KEY_OPTION, true));
        }
        out.push(Step::Key(key, true));
        out.push(Step::Key(key, false));
        if option {
            out.push(Step::Key(KEY_OPTION, false));
        }
        if shift {
            out.push(Step::Key(KEY_SHIFT, false));
        }
    }
    Ok(out)
}

fn save_png(path: &str, frame: &(u16, u16, Vec<u8>)) -> Result<()> {
    let mut encoder = png::Encoder::new(File::create(path)?, frame.0 as u32, frame.1 as u32);
    encoder.set_color(png::ColorType::Rgba);
    encoder.set_depth(png::BitDepth::Eight);
    encoder.write_header()?.write_image_data(&frame.2)?;
    Ok(())
}

fn contains(haystack: &[u8], needle: &str) -> bool {
    haystack.windows(needle.len()).any(|w| w == needle.as_bytes())
}

fn main() -> Result<()> {
    env_logger::Builder::from_default_env()
        .filter_level(log::LevelFilter::Warn)
        .init();
    let args = Args::parse();

    let rom = fs::read(&args.rom).with_context(|| format!("reading ROM {}", args.rom))?;
    let model = MacModel::detect_from_rom(&rom).context("cannot detect Mac model from ROM")?;

    let (mut emulator, frame_recv) = Emulator::new(&rom, &[], model)?;
    let cmd = emulator.create_cmd_sender();
    let event_recv = emulator.create_event_recv();

    if let Some(floppy) = args.floppy {
        cmd.send(EmulatorCommand::InsertFloppy(0, floppy, true))?;
    }
    for spec in &args.hdd {
        let (id, path) = spec.split_once(':').context("--hdd wants ID:PATH")?;
        cmd.send(EmulatorCommand::ScsiAttachHdd(id.parse()?, PathBuf::from(path)))?;
    }
    // SSH OS reads the mouse from the hardware; Snow's default mode instead
    // writes the position into Mac OS's memory.
    cmd.send(EmulatorCommand::SetMouseMode(MouseMode::RelativeHw))?;
    if let Some(id) = args.net {
        cmd.send(EmulatorCommand::ScsiAttachEthernet(id))?;
        if let Some(pcap) = &args.pcap {
            cmd.send(EmulatorCommand::EthernetStartCapture(id, PathBuf::from(pcap)))?;
        }
    }
    cmd.send(EmulatorCommand::Run)?;
    cmd.send(EmulatorCommand::SetSpeed(EmulatorSpeed::Uncapped))?;

    let mut serial: Vec<u8> = Vec::new();
    let mut last_frame: Option<(u16, u16, Vec<u8>)> = None;
    let mut stop_at = args.cycles;
    let mut marker_seen = false;
    let mut stopped = false;

    // Typing: a queue of key transitions, sent one at a time once the
    // --until marker has appeared.
    let mut keys = match args.r#type.as_deref() {
        Some(text) => keystrokes(text)?.into_iter().collect::<std::collections::VecDeque<_>>(),
        None => Default::default(),
    };
    let typing = !keys.is_empty();
    let mut next_key_at: Ticks = 0;
    // Set after Return is typed: where in the serial log to look for the
    // next prompt before typing on.
    let mut await_prompt_from: Option<usize> = None;
    let mut return_pressed_at: usize = 0;
    let mut shots = 0;
    let mut typing_finished = false;
    // Where in the serial log the typing started; --done only looks after it.
    let mut typing_from: Option<usize> = None;
    let mut done_seen = false;

    while emulator.get_cycles() < stop_at {
        if let Some(buf) = frame_recv.lock().unwrap().take() {
            last_frame = Some((buf.width(), buf.height(), buf.into_inner()));
        }
        while let Ok(event) = event_recv.try_recv() {
            match event {
                EmulatorEvent::SccTransmitData(SccCh::A, data) => {
                    serial.extend_from_slice(&data);
                    if let (false, Some(marker)) = (marker_seen, args.until.as_ref()) {
                        if contains(&serial, marker) {
                            marker_seen = true;
                            if typing {
                                typing_from = Some(serial.len());
                                next_key_at = emulator.get_cycles() + SETTLE_CYCLES;
                            } else {
                                stop_at = stop_at.min(emulator.get_cycles() + SETTLE_CYCLES);
                            }
                        }
                    }
                    // Only output produced after the typing started counts as "done".
                    if let (false, Some(from), Some(marker)) =
                        (done_seen, typing_from, args.done.as_ref())
                    {
                        if contains(&serial[from..], marker) {
                            done_seen = true;
                            stop_at = stop_at.min(emulator.get_cycles() + SETTLE_CYCLES);
                        }
                    }
                }
                EmulatorEvent::Status(s) => {
                    if !s.running && s.cycles > 100 {
                        stopped = true;
                    }
                }
                _ => (),
            }
        }
        if stopped {
            break;
        }
        if let Some(from) = await_prompt_from {
            let tail = &serial[from.min(serial.len())..];
            let found = if args.prompt.is_empty() {
                contains(tail, "> ")
            } else {
                args.prompt.iter().any(|p| contains(tail, p))
            };
            if found {
                await_prompt_from = None;
                next_key_at = emulator.get_cycles() + KEY_CYCLES;
            }
        }
        if marker_seen
            && typing
            && !typing_finished
            && await_prompt_from.is_none()
            && emulator.get_cycles() >= next_key_at
        {
            match keys.pop_front() {
                Some(Step::Key(key, true)) => {
                    // A quick command prints its prompt before Return is
                    // even released, so note the place now.
                    if key == KEY_RETURN {
                        return_pressed_at = serial.len();
                    }
                    cmd.send(EmulatorCommand::KeyEvent(KeyEvent::KeyDown(key, Keymap::AekM0115)))?
                }
                Some(Step::Key(key, false)) => {
                    cmd.send(EmulatorCommand::KeyEvent(KeyEvent::KeyUp(key, Keymap::AekM0115)))?;
                    if key == KEY_RETURN && !keys.is_empty() {
                        await_prompt_from = Some(return_pressed_at);
                    }
                }
                Some(Step::Mouse(relx, rely)) => {
                    cmd.send(EmulatorCommand::MouseUpdateRelative { relx, rely, btn: None })?
                }
                Some(Step::Button(down)) => cmd.send(EmulatorCommand::MouseUpdateRelative {
                    relx: 0,
                    rely: 0,
                    btn: Some(down),
                })?,
                Some(Step::Pause) => {
                    next_key_at = emulator.get_cycles() + 8_000_000;
                    continue;
                }
                Some(Step::Shot) => {
                    shots += 1;
                    if let Some(frame) = &last_frame {
                        save_png(&format!("{}-{}.png", args.out, shots), frame)?;
                    }
                }
                None => {
                    typing_finished = true;
                    if args.done.is_none() {
                        stop_at = stop_at.min(emulator.get_cycles() + 10 * SETTLE_CYCLES);
                    }
                }
            }
            next_key_at = emulator.get_cycles() + KEY_CYCLES;
        }
        emulator.tick(1, ())?;
    }

    fs::write(format!("{}.serial.txt", args.out), &serial)?;
    if let Some(frame) = &last_frame {
        save_png(&format!("{}.png", args.out), frame)?;
    }

    println!(
        "model={} cycles={} serial_bytes={} marker={} cpu_stopped={}",
        model,
        emulator.get_cycles(),
        serial.len(),
        match (&args.until, marker_seen, &args.done, done_seen) {
            (None, _, _, _) => "none",
            (Some(_), false, _, _) => "NOT SEEN",
            (Some(_), true, Some(_), false) => "seen, but --done NOT SEEN",
            (Some(_), true, _, _) => "seen",
        },
        stopped
    );
    if (args.until.is_some() && !marker_seen) || (args.done.is_some() && !done_seen) {
        std::process::exit(3);
    }
    Ok(())
}
