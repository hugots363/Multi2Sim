#!/bin/bash

plots_dir=./plots
gnuplot_tmp=./plot.tmp.gp

mkdir -p ${plots_dir}


while read bench; do
	echo "#!/usr/bin/gnuplot -persist
	set key left top
	set title \"${bench}\"
	set xlabel \"Cycles\"
	set ylabel \"IPC\"
	set format x \"%2.0t{/Symbol \327}10^{%L}\"
	set terminal pngcairo enhanced font \"Verdana,10\"
	set output \"${bench}.png\"" > plot.tmp.gp

	first=true
	for dir in *; do
		if ! [ -d $dir ]; then
			continue
		fi

		if ! [ -f ${dir}/${bench}.ipc ]; then
			echo Aviso: No existe ${bench}.ipc en ${dir}
			continue
		fi
		
		if $first ; then
			echo -n "plot \"${dir}/${bench}.ipc\" every ::25 using 1:4 with lines title \"${dir}\"" >> plot.tmp.gp
			first=false
		else
			echo -n ", \"${dir}/${bench}.ipc\" every ::25 using 1:4 with lines title \"${dir}\"" >> plot.tmp.gp
		fi
	done
	echo >> plot.tmp.gp
	gnuplot plot.tmp.gp
done < benchlist.txt


