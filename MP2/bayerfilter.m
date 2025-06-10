Width = 1920;
Height = 1080;
inputImage = imread('cat1080p.bmp', 'bmp');
bayerFilter = inputImage;
disp(size(inputImage));
% Red is 1
% Green is 2
% Blue is 3
disp(redRow);
for i = 1:Height
    for j = 1:Width
        redRow = mod(i,2) == 1;
        if(redRow)
            if(mod(j, 2) == 1)
            %red row and green pixel
                bayerFilter(i,j,1) = bitand(inputImage(i,j,1),1);
                bayerFilter(i,j,3) = bitand(inputImage(i,j,3),1);
            else
            %red row and red pixel
                bayerFilter(i,j,2) = bitand(inputImage(i,j,2),1);
                bayerFilter(i,j,3) = bitand(inputImage(i,j,3),1);                
            end
        else
            if(mod(j,2) == 0)
            %blue row and green pixel
                bayerFilter(i,j,1) = bitand(inputImage(i,j,1),1);
                bayerFilter(i,j,3) = bitand(inputImage(i,j,3),1);
            else
            %blue row and blue pixel 
                bayerFilter(i,j,1) = bitand(inputImage(i,j,1),1);
                bayerFilter(i,j,2) = bitand(inputImage(i,j,2),1);                
            end
        end

    end
end
imwrite(bayerFilter, 'cat1080p_bayerTest.bmp', 'bmp');

