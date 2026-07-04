# change overview
Add a new view panel with related config panel for calculating and viewing an average spectrum.
Panel names: "Average View" - display spectrum, "Average" - configuration.

# general functionality description for "average view"
Plotting panel looking the same as Spectrum View with some independent controls.

When avarge spectrum data is not available then display centered, large text "No average spectrum available" in place of the plot.

For average spectrum use the yellow plotting color, same as in the spectrum view.

When average spectrum is displayed, in the top right corner of the panel write "Average of N" where N is the number of averaged data records.

Average view is cleared when another dataset is loaded.

# general functionality description for "Average"
Configuration/settings panel for averaging functionality.

In the top of the panel there is button saying "Calculate average". When it is pressed, average spectrum gets calculated. Spectra are calculated for all selected data files and then averaged. Only use selected data for calculation. Only calculate average when this button is pressed.

Below that place 2 more buttons, stacked horizontally with a label. Label says "Select" and buttons say "All" and "None". When "All" is pressed, select all data in "Files". When "None" then deselect all.

Y scale, X unit and Y axis settings are doubled in average view and independent from each other. 

Both average and spectrum config panels display the cursor on/off selector but this one is synchronized between them. If it is toggled in one panel it automatically adjusts in the other. 

# modification of "Files" panel
Each file now has a corresponding checkbox in the same row that allows to select it for averaging. Align checkboxes to the right and data file names to the left. 

By default after loading a dataset, all checkboxes are checked (i.e. all data selected for averaging).

checked boxes are white and unchecked boxes are gray.




