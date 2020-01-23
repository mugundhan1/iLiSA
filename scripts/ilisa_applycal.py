#!/usr/bin/python
"""Apply a calibration file to ACC or XST data folder.
"""

import argparse

import ilisa.calim.calibration


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('cvcpath',
                        help="""Path to CVC folder""")
    parser.add_argument('caltabpath', help="""Path to caltab file""")
    args = parser.parse_args()

    ilisa.calim.calibration.cvcfolder_applycal(args.cvcpath, args.caltabpath)
