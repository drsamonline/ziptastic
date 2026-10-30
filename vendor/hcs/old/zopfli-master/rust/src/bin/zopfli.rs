//! Command line binary implementation of the Zopfli compressor.

#![forbid(unsafe_code)]

use std::env;
use std::fs;
use std::io::{self, Write};
use std::process;
use zopfli_rs::deflate::deflate;
use zopfli_rs::gzip_container::gzip_compress;
use zopfli_rs::util::SafeOptions;
use zopfli_rs::zlib_container::zlib_compress;
use zopfli_rs::BlockType;

const MAX_FILE_SIZE: u64 = i32::MAX as u64; // 2GB limit (i32::MAX)

fn print_help() {
    let help_text = "\
Usage: zopfli [OPTION]... FILE...
  -h    gives this help
  -c    write the result on standard output, instead of disk filename + '.gz'
  -v    verbose mode
  --i#  perform # iterations (default 15). More gives more compression but is slower. Examples: --i10, --i50, --i1000
  --gzip        output to gzip format (default)
  --zlib        output to zlib format instead of gzip
  --deflate     output to deflate format instead of gzip
  --splitlast   ignored, left for backwards compatibility\n";
    eprint!("{}", help_text);
}

fn compress_file(
    options: &SafeOptions,
    format: &str,
    in_filename: &str,
    out_filename: Option<&str>,
) {
    let metadata = match fs::metadata(in_filename) {
        Ok(m) => m,
        Err(_) => {
            eprintln!("Invalid filename: {}", in_filename);
            return;
        }
    };

    if metadata.len() > MAX_FILE_SIZE {
        eprintln!("Files larger than 2GB are not supported.");
        process::exit(1);
    }

    let in_data = match fs::read(in_filename) {
        Ok(data) => data,
        Err(_) => {
            eprintln!("Invalid filename: {}", in_filename);
            return;
        }
    };

    if in_data.len() as u64 > MAX_FILE_SIZE {
        eprintln!("Files larger than 2GB are not supported.");
        process::exit(1);
    }

    let mut out_buffer = Vec::new();
    let compress_result = match format {
        "gzip" => gzip_compress(options, &in_data, &mut out_buffer),
        "zlib" => zlib_compress(options, &in_data, &mut out_buffer),
        "deflate" => {
            let mut bp = 0u8;
            deflate(options, BlockType::DynamicTree, true, &in_data, &mut bp, &mut out_buffer)
        }
        _ => unreachable!(),
    };

    if let Err(e) = compress_result {
        eprintln!("Error compressing {}: {}", in_filename, e);
        return;
    }

    if let Some(out_name) = out_filename {
        if fs::write(out_name, &out_buffer).is_err() {
            eprintln!("Error: Cannot write to output file, terminating.");
            process::exit(1);
        }
    } else if io::stdout().write_all(&out_buffer).is_err() {
        eprintln!("Error: Cannot write to output file, terminating.");
        process::exit(1);
    }
}

fn main() {
    let args: Vec<String> = env::args().collect();
    let program_name = if args.is_empty() { "zopfli".to_string() } else { args[0].clone() };

    let mut verbose = false;
    let mut output_to_stdout = false;
    let mut output_type = "gzip";
    let mut numiterations = 15;
    let mut files = Vec::new();

    for arg in args.iter().skip(1) {
        if arg.starts_with('-') {
            if arg == "-h" {
                print_help();
                process::exit(0);
            } else if arg == "-v" {
                verbose = true;
            } else if arg == "-c" {
                output_to_stdout = true;
            } else if arg == "--deflate" {
                output_type = "deflate";
            } else if arg == "--zlib" {
                output_type = "zlib";
            } else if arg == "--gzip" {
                output_type = "gzip";
            } else if arg == "--splitlast" {
                // Ignore
            } else if arg.starts_with("--i")
                && arg.chars().nth(3).is_some_and(|c| c.is_ascii_digit())
            {
                let num_str: String =
                    arg.chars().skip(3).take_while(|c| c.is_ascii_digit()).collect();
                numiterations = num_str.parse::<i32>().unwrap_or(15);
            }
        } else {
            files.push(arg.clone());
        }
    }

    if numiterations < 1 {
        eprintln!("Error: must have 1 or more iterations");
        process::exit(1);
    }

    if files.is_empty() {
        eprintln!("Please provide filename\nFor help, type: {} -h", program_name);
        process::exit(0);
    }

    let options = SafeOptions {
        verbose,
        verbose_more: false,
        numiterations,
        blocksplitting: true,
        blocksplittinglast: false,
        blocksplittingmax: 15,
    };

    for filename in &files {
        let out_filename = if output_to_stdout {
            None
        } else {
            match output_type {
                "gzip" => Some(format!("{}.gz", filename)),
                "zlib" => Some(format!("{}.zlib", filename)),
                "deflate" => Some(format!("{}.deflate", filename)),
                _ => unreachable!(),
            }
        };

        if let (true, Some(name)) = (verbose, &out_filename) {
            eprintln!("Saving to: {}", name);
        }

        compress_file(&options, output_type, filename, out_filename.as_deref());
    }
}
